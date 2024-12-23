#include "ps2.h"

#include <base/types.h>
#include <data/ring.h>
#include <log/log.h>
#include <x86/asm.h>

#include "arch/idt.h"
#include "arch/irq.h"
#include "input/kbd.h"
#include "sys/keyboard.h"

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

static bool extended = false;


static void ps2_irq_handler(UNUSED int_state* s) {
    u8 scancode = inb(0x60);

    if (scancode == PS2_EXTENDED) {
        extended = true;
        goto done;
    }

    bool released = false;

    if (scancode >= PS2_RELEASE_OFFSET) {
        scancode -= PS2_RELEASE_OFFSET;
        released = true;
    }

    u8 code = extended ? ps2_codes_extended[scancode] : ps2_codes[scancode];

    kbd_handle_key((key_event){
        .type = !released,
        .code = code,
    });

#ifdef PS2_DEBUG
    log_debug("[PS2 DEBUG] scancode = %#x, extended = %u", scancode, extended);
#endif

    if (extended)
        extended = false;

done:
    irq_ack(IRQ_PS2_KEYBOARD);
}


void init_ps2_kbd() {
    irq_register(IRQ_PS2_KEYBOARD, ps2_irq_handler);
}
