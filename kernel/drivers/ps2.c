#include "ps2.h"

#include <base/types.h>
#include <data/ring.h>
#include <input/kbd.h>
#include <input/mouse.h>
#include <log/log.h>
#include <stddef.h>
#include <x86/asm.h>

#include "arch/idt.h"
#include "arch/irq.h"
#include "sys/keyboard.h"
#include "sys/mouse.h"

// Code set number 1
static const u8 ps2_codes[128] = {
    0,
    KBD_ESCAPE,
    KBD_1,
    KBD_2,
    KBD_3,
    KBD_4,
    KBD_5,
    KBD_6,
    KBD_7,
    KBD_8,
    KBD_9,
    KBD_0,
    KBD_MINUS,
    KBD_EQUALS,
    KBD_BACKSPACE,
    KBD_TAB,
    KBD_Q,
    KBD_W,
    KBD_E,
    KBD_R,
    KBD_T,
    KBD_Y,
    KBD_U,
    KBD_I,
    KBD_O,
    KBD_P,
    KBD_LEFT_BRACKET,
    KBD_RIGHT_BRACKET,
    KBD_ENTER,
    KBD_LEFT_CTRL,
    KBD_A,
    KBD_S,
    KBD_D,
    KBD_F,
    KBD_G,
    KBD_H,
    KBD_J,
    KBD_K,
    KBD_L,
    KBD_SEMICOLON,
    KBD_QUOTE,
    KBD_BACKTICK,
    KBD_LEFT_SHIFT,
    KBD_BACKSLASH,
    KBD_Z,
    KBD_X,
    KBD_C,
    KBD_V,
    KBD_B,
    KBD_N,
    KBD_M,
    KBD_COMMA,
    KBD_DOT,
    KBD_SLASH,
    KBD_RIGHT_SHIFT,
    KBD_KP_MULTIPLY,
    KBD_LEFT_ALT,
    KBD_SPACE,
    KBD_CAPSLOCK,
    KBD_F1,
    KBD_F2,
    KBD_F3,
    KBD_F4,
    KBD_F5,
    KBD_F6,
    KBD_F7,
    KBD_F8,
    KBD_F9,
    KBD_F10,
    KBD_NUMLOCK,
    KBD_SCRLLOCK,
    KBD_KP_7,
    KBD_KP_8,
    KBD_KP_9,
    KBD_KP_MINUS,
    KBD_KP_4,
    KBD_KP_5,
    KBD_KP_6,
    KBD_KP_PLUS,
    KBD_KP_1,
    KBD_KP_2,
    KBD_KP_3,
    KBD_KP_0,
    KBD_KP_PERIOD,
    0,
    0,
    0,
    KBD_F11,
    KBD_F12,
};

static const u8 ps2_codes_extended[128] = {
    [0x1c] = KBD_KP_ENTER,
    [0x1d] = KBD_RIGHT_CTRL,
    [0x35] = KBD_KP_DIVIDE,
    [0x38] = KBD_RIGHT_ALT,
    [0x48] = KBD_UP,
    [0x49] = KBD_PAGEUP,
    [0x4b] = KBD_LEFT,
    [0x4d] = KBD_RIGHT,
    [0x4f] = KBD_END,
    [0x50] = KBD_DOWN,
    [0x51] = KBD_PAGEDOWN,
    [0x52] = KBD_INSERT,
    [0x53] = KBD_DELETE,
    [0x5b] = KBD_LEFT_SUPER,
    [0x5c] = KBD_RIGHT_SUPER,
};


static bool has_port1 = false; // keyboard
static bool has_port2 = false; // mouse


// buffer == 1 waits for the input buffer and buffer == 0 waits for the output buffer
static bool _wait_buffer(bool buffer) {
    u8 mask = buffer ? PS2_STA_IN_BUFFER : PS2_STA_OUT_BUFFER;

    for (usize timeout = 10000; timeout > 0; timeout--) {
        u8 status = inb(PS2_REG_STATUS);

        if (!(status & mask))
            return 0;
    }

    return 1;
}

static bool _ps2_command(u8 com) {
    if (_wait_buffer(1))
        return false;

    outb(PS2_REG_COMMAND, com);
    return true;
}

static bool _ps2_data(u8 data) {
    if (_wait_buffer(1))
        return false;

    outb(PS2_REG_DATA, data);
    return true;
}

static u8 _ps2_response(void) {
    _wait_buffer(0);
    return inb(PS2_REG_DATA);
}

static bool _flush_output_buffer(void) {
    for (usize timeout = 10000; timeout > 0; timeout--) {
        u8 status = inb(PS2_REG_STATUS);

        if (!(status & PS2_STA_OUT_BUFFER)) // The buffer is clear
            return 0;

        inb(PS2_REG_DATA);
    }

    return 1;
}


static bool kbd_command(u8 com) {
    _ps2_data(com);
    _ps2_response(); // check for ACK?

    return true;
}

static bool mouse_command(u8 com) {
    outb(PS2_REG_COMMAND, PS2_MOUSE_SPECIFIER);

    _ps2_data(com);
    _ps2_response(); // check for ACK?

    return true;
}


static bool probe_port(usize port) {
    _ps2_command((port == 1) ? PS2_COM_ENABLE_PORT1 : PS2_COM_ENABLE_PORT2);

    u8 bit = (port == 1) ? PS2_CON_PORT1_CLOCK : PS2_CON_PORT2_CLOCK;
    if (inb(PS2_COM_READ_CONFIG) & bit)
        return false;

    return true;
}

static bool init_controller(void) {
    _ps2_command(PS2_COM_DISABLE_PORT1);
    _ps2_command(PS2_COM_DISABLE_PORT2);

    if (_flush_output_buffer())
        return false;

    _ps2_command(PS2_COM_READ_CONFIG);

    u8 conf = _ps2_response();
    conf |= PS2_CON_PORT1_IRQ | PS2_CON_PORT2_IRQ | PS2_CON_PORT1_TRANSLATE;

    _ps2_command(PS2_COM_WRITE_CONFIG);
    _ps2_data(conf);

    _ps2_command(PS2_COM_ENABLE_PORT1);
    _ps2_command(PS2_COM_ENABLE_PORT2);

    return true;
}


static u8 kbd_index = 0;
static bool kbd_extended = false;

static void ps2_kbd_irq(UNUSED int_state* s) {
    u8 scancode = inb(0x60);

    if (scancode == PS2_EXTENDED) {
        kbd_extended = true;
        goto done;
    }

    bool released = false;

    if (scancode >= PS2_RELEASE_OFFSET) {
        scancode -= PS2_RELEASE_OFFSET;
        released = true;
    }

    key_event event = {
        .source = kbd_index,
        .type = !released ? KEY_ACTION : 0,
        .code = kbd_extended ? ps2_codes_extended[scancode] : ps2_codes[scancode],
    };

    kbd_handle_key(event);

    if (kbd_extended)
        kbd_extended = false;

done:
    irq_ack(IRQ_PS2_KEYBOARD);
}


static u8 mouse_index = 0;

static u8 mouse_byte = 0; // On mouse events three irqs are sent. One for each byte of the packet
static u8 mouse_packet_size = 3; // With mouse scroll packets are 4 bytes large

static u8 mouse_packet[4];

static void ps2_mouse_irq(UNUSED int_state* s) {
    u8 packet = inb(0x60);

    mouse_packet[mouse_byte++] = packet;

    // We are done parsing this packet
    if (mouse_byte == mouse_packet_size) {
        mouse_byte = 0;

        u8 flags = mouse_packet[0];

        i16 x = mouse_packet[1];
        i16 y = mouse_packet[2];

        // TODO: overflow bit?

        // Check the sign bit
        if (flags & PS2_MOUSE_XSIGN)
            x -= 0x100;

        if (flags & PS2_MOUSE_YSIGN)
            y -= 0x100;

        // TODO: z axis / scroll

        u8 buttons = 0;

        if (flags & PS2_MOUSE_LEFT)
            buttons |= MOUSE_LEFT_CLICK;

        if (flags & PS2_MOUSE_RIGHT)
            buttons |= MOUSE_RIGHT_CLICK;

        if (flags & PS2_MOUSE_MIDDLE)
            buttons |= MOUSE_MIDDLE_CLICK;

        mouse_event event = {
            .delta_x = x,
            .delta_y = -y, // flip the y axis so that we end up in Q4
            .buttons = buttons,
            .source = mouse_index,
        };

        mouse_handle_event(event);
    }

    irq_ack(IRQ_PS2_MOUSE);
}


void init_ps2() {
    has_port1 = probe_port(1);
    has_port2 = probe_port(2);

    if (!has_port1 && !has_port2)
        return;

    if (!init_controller()) {
        log_warn("Unable to initialise the PS2 controller");
        return;
    }

    if (has_port1) {
        // Tell the keyboard to use code set 1
        // We set it to 2 but we end up getting codes from set 1 since we enabled translation
        // This is a thing for historic reasons (see OSdev Wiki for more info)
        kbd_command(PS2_KBD_COM_SET_CODESET);
        kbd_command(2);

        irq_register(IRQ_PS2_KEYBOARD, ps2_kbd_irq);

        kbd_index = register_keyboard("PS/2 keyboard", NULL);
    }

    if (has_port2) {
        mouse_command(PS2_MOUSE_COM_DEFAULT);
        mouse_command(PS2_MOUSE_COM_ENABLE_DATA);

        irq_register(IRQ_PS2_MOUSE, ps2_mouse_irq);

        mouse_index = register_mouse("PS/2 mouse");
    }
}
