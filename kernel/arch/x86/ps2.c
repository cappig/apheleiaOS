#include "ps2.h"

#include <base/attributes.h>
#include <base/types.h>
#include <input/kbd.h>
#include <input/mouse.h>
#include <log/log.h>
#include <sys/keyboard.h>
#include <sys/mouse.h>
#include <x86/asm.h>
#include <x86/irq.h>

#define PS2_RELEASE_OFFSET 0x80
#define PS2_EXTENDED       0xe0
#define PS2_ACK            0xfa
#define PS2_SELFTEST_OK    0x55
#define PS2_TEST_OK        0x00

#define PS2_WAIT_LIMIT 100000

#define IRQ_PS2_KEYBOARD 1
#define IRQ_PS2_MOUSE    12

enum ps2_ports {
    PS2_REG_DATA = 0x60,
    PS2_REG_STATUS = 0x64,
    PS2_REG_COMMAND = 0x64,
};

enum ps2_status_bits {
    PS2_STA_OUT_BUFFER = 1 << 0,
    PS2_STA_IN_BUFFER = 1 << 1,
    PS2_STA_AUX = 1 << 5,
    PS2_STA_TIME_OUT_ERROR = 1 << 6,
    PS2_STA_PARITY_ERROR = 1 << 7,
};

enum ps2_control_bits {
    PS2_CON_PORT1_IRQ = 1 << 0,
    PS2_CON_PORT2_IRQ = 1 << 1,
    PS2_CON_PORT1_CLOCK = 1 << 4,
    PS2_CON_PORT2_CLOCK = 1 << 5,
    PS2_CON_PORT1_TRANSLATE = 1 << 6,
};

enum ps2_commands {
    PS2_COM_READ_CONFIG = 0x20,
    PS2_COM_WRITE_CONFIG = 0x60,

    PS2_COM_DISABLE_PORT2 = 0xa7,
    PS2_COM_ENABLE_PORT2 = 0xa8,
    PS2_COM_TEST_PORT2 = 0xa9,

    PS2_COM_TEST_CONTROLLER = 0xaa,

    PS2_COM_TEST_PORT1 = 0xab,
    PS2_COM_DISABLE_PORT1 = 0xad,
    PS2_COM_ENABLE_PORT1 = 0xae,
};

enum ps2_kbd_commands {
    PS2_KBD_COM_SET_CODESET = 0xf0,
    PS2_KBD_COM_ENABLE_SCAN = 0xf4,
};

enum ps2_mouse_commands {
    PS2_MOUSE_COM_DEFAULT = 0xf6,
    PS2_MOUSE_COM_ENABLE_DATA = 0xf4,
};

enum ps2_mouse_flags {
    PS2_MOUSE_LEFT = 1 << 0,
    PS2_MOUSE_RIGHT = 1 << 1,
    PS2_MOUSE_MIDDLE = 1 << 2,
    PS2_MOUSE_XSIGN = 1 << 4,
    PS2_MOUSE_YSIGN = 1 << 5,
    PS2_MOUSE_XOVERFLOW = 1 << 6,
    PS2_MOUSE_YOVERFLOW = 1 << 7,
};

// Code set 1
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

static bool has_port1 = false;
static bool has_port2 = false;

static bool _wait_input_clear(void) {
    for (size_t timeout = PS2_WAIT_LIMIT; timeout > 0; timeout--) {
        u8 status = inb(PS2_REG_STATUS);

        if (!(status & PS2_STA_IN_BUFFER)) {
            return true;
        }
    }

    return false;
}

static bool _wait_output_full(void) {
    for (size_t timeout = PS2_WAIT_LIMIT; timeout > 0; timeout--) {
        u8 status = inb(PS2_REG_STATUS);

        if (status & PS2_STA_OUT_BUFFER) {
            return true;
        }
    }

    return false;
}

static bool _write_cmd(u8 cmd) {
    if (!_wait_input_clear()) {
        return false;
    }

    outb(PS2_REG_COMMAND, cmd);

    return true;
}

static bool _write_data(u8 data) {
    if (!_wait_input_clear()) {
        return false;
    }

    outb(PS2_REG_DATA, data);

    return true;
}

static bool _read_data(u8 *out) {
    if (!out) {
        return false;
    }

    if (!_wait_output_full()) {
        return false;
    }

    *out = inb(PS2_REG_DATA);
    return true;
}

static void _flush_output(void) {
    for (;;) {
        u8 status = inb(PS2_REG_STATUS);

        if (!(status & PS2_STA_OUT_BUFFER)) {
            break;
        }

        (void)inb(PS2_REG_DATA);
    }
}

static bool _expect_ack(void) {
    u8 resp = 0;
    if (!_read_data(&resp)) {
        return false;
    }

    return resp == PS2_ACK;
}

static bool _kbd_command(u8 cmd) {
    if (!_write_data(cmd)) {
        return false;
    }

    return _expect_ack();
}

static bool _mouse_command(u8 cmd) {
    if (!_write_cmd(0xd4)) {
        return false;
    }

    if (!_write_data(cmd)) {
        return false;
    }

    return _expect_ack();
}

static bool _controller_init(void) {
    if (!_write_cmd(PS2_COM_DISABLE_PORT1)) {
        return false;
    }

    if (!_write_cmd(PS2_COM_DISABLE_PORT2)) {
        return false;
    }

    _flush_output();

    if (!_write_cmd(PS2_COM_READ_CONFIG)) {
        return false;
    }

    u8 config = 0;
    if (!_read_data(&config)) {
        return false;
    }

    config &= ~(PS2_CON_PORT1_IRQ | PS2_CON_PORT2_IRQ);
    config |= PS2_CON_PORT1_TRANSLATE;

    if (!_write_cmd(PS2_COM_WRITE_CONFIG)) {
        return false;
    }

    if (!_write_data(config)) {
        return false;
    }

    if (_write_cmd(PS2_COM_TEST_CONTROLLER)) {
        u8 resp = 0;

        if (!_read_data(&resp) || resp != PS2_SELFTEST_OK) {
            log_warn("ps2: controller self-test failed");
        }
    }

    if (_write_cmd(PS2_COM_TEST_PORT1)) {
        u8 resp = 0;

        if (_read_data(&resp)) {
            has_port1 = (resp == PS2_TEST_OK);
        }
    }

    if (_write_cmd(PS2_COM_TEST_PORT2)) {
        u8 resp = 0;

        if (_read_data(&resp)) {
            has_port2 = (resp == PS2_TEST_OK);
        }
    }

    if (has_port1) {
        _write_cmd(PS2_COM_ENABLE_PORT1);
    }

    if (has_port2) {
        _write_cmd(PS2_COM_ENABLE_PORT2);
    }

    if (has_port1) {
        config |= PS2_CON_PORT1_IRQ;
        config &= ~PS2_CON_PORT1_CLOCK;
    } else {
        config |= PS2_CON_PORT1_CLOCK;
    }

    if (has_port2) {
        config |= PS2_CON_PORT2_IRQ;
        config &= ~PS2_CON_PORT2_CLOCK;
    } else {
        config |= PS2_CON_PORT2_CLOCK;
    }

    if (!_write_cmd(PS2_COM_WRITE_CONFIG)) {
        return false;
    }

    if (!_write_data(config)) {
        return false;
    }

    return true;
}

static u8 kbd_index = 0;
static bool kbd_extended = false;

static void _kbd_irq(UNUSED int_state_t *s) {
    u8 scancode = inb(PS2_REG_DATA);

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

    keyboard_handle_key(event);

    if (kbd_extended) {
        kbd_extended = false;
    }

done:
    irq_ack(IRQ_PS2_KEYBOARD);
}

static u8 mouse_index = 0;
static u8 mouse_byte = 0;
static u8 mouse_packet[4];
static u8 mouse_packet_size = 3;

static void _mouse_irq(UNUSED int_state_t *s) {
    u8 status = inb(PS2_REG_STATUS);
    u8 packet = inb(PS2_REG_DATA);

    if (!(status & PS2_STA_AUX)) {
        goto done;
    }

    if (!mouse_byte && !(packet & 0x08)) {
        goto done;
    }

    mouse_packet[mouse_byte++] = packet;

    if (mouse_byte == mouse_packet_size) {
        mouse_byte = 0;

        u8 flags = mouse_packet[0];

        if (flags & (PS2_MOUSE_XOVERFLOW | PS2_MOUSE_YOVERFLOW)) {
            goto done;
        }

        i16 x = mouse_packet[1];
        i16 y = mouse_packet[2];

        if (flags & PS2_MOUSE_XSIGN) {
            x -= 0x100;
        }

        if (flags & PS2_MOUSE_YSIGN) {
            y -= 0x100;
        }

        u8 buttons = 0;

        if (flags & PS2_MOUSE_LEFT) {
            buttons |= MOUSE_LEFT_CLICK;
        }

        if (flags & PS2_MOUSE_RIGHT) {
            buttons |= MOUSE_RIGHT_CLICK;
        }

        if (flags & PS2_MOUSE_MIDDLE) {
            buttons |= MOUSE_MIDDLE_CLICK;
        }

        mouse_event event = {
            .delta_x = x,
            .delta_y = (i16)-y,
            .buttons = buttons,
            .source = mouse_index,
        };

        mouse_handle_event(event);
    }

done:
    irq_ack(IRQ_PS2_MOUSE);
}

void ps2_init(void) {
    if (!_controller_init()) {
        log_warn("ps2: controller init failed");
        return;
    }

    if (!has_port1 && !has_port2) {
        log_info("ps2: no devices detected");
        return;
    }

    if (has_port1) {
        _kbd_command(PS2_KBD_COM_SET_CODESET);
        _kbd_command(2);
        _kbd_command(PS2_KBD_COM_ENABLE_SCAN);

        irq_register(IRQ_PS2_KEYBOARD, _kbd_irq);

        kbd_index = keyboard_register("PS/2 keyboard", NULL);
    }

    if (has_port2) {
        _mouse_command(PS2_MOUSE_COM_DEFAULT);
        _mouse_command(PS2_MOUSE_COM_ENABLE_DATA);

        irq_register(IRQ_PS2_MOUSE, _mouse_irq);

        mouse_index = mouse_register("PS/2 mouse");
    }

    log_info("ps2: controller ready");
}
