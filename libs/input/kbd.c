#include "kbd.h"

#include <ctype.h>

#include "input/keymap.h"

static const u8 ctrl_ascii[6] = {'\n', '\n', '\b', '\t', '\e', 0x7f};


char kbd_to_ascii(key_event event, ascii_keymap map, bool shift) {
    u8 code = event.code;

    switch (code) {
    // Mapped printable keys
    case 1 ... 63:
        return map[shift][code];

    // ascii control codes
    case KBD_KP_ENTER ... KBD_DELETE:
        return ctrl_ascii[code - KBD_KP_ENTER];

    default:
        return '\0';
    }
}

char kbd_to_ascii_default(key_event event) {
    return kbd_to_ascii(event, us_keymap, false);
}


char ctrl_to_caret(char ascii) {
    if (!iscntrl(ascii))
        return 0;

    if (ascii == 127)
        return '?';

    return '@' + ascii;
}
