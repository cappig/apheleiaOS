#pragma once

#include <base/types.h>

#include "input/keymap.h"

// These are essentially just PS2 codes from set 1
// https://wiki.osdev.org/PS/2_Keyboard

enum kbd_codes {
    KBD_NOTHING = 0, // Should be ignored

    KBD_A = 1,
    KBD_B = 2,
    KBD_C = 3,
    KBD_D = 4,
    KBD_E = 5,
    KBD_F = 6,
    KBD_G = 7,
    KBD_H = 8,
    KBD_I = 9,
    KBD_J = 10,
    KBD_K = 11,
    KBD_L = 12,
    KBD_M = 13,
    KBD_N = 14,
    KBD_O = 15,
    KBD_P = 16,
    KBD_Q = 17,
    KBD_R = 18,
    KBD_S = 19,
    KBD_T = 20,
    KBD_U = 21,
    KBD_V = 22,
    KBD_W = 23,
    KBD_X = 24,
    KBD_Y = 25,
    KBD_Z = 26,

    KBD_0 = 27,
    KBD_1 = 28,
    KBD_2 = 29,
    KBD_3 = 30,
    KBD_4 = 31,
    KBD_5 = 32,
    KBD_6 = 33,
    KBD_7 = 34,
    KBD_8 = 35,
    KBD_9 = 36,

    KBD_MINUS = 37,
    KBD_EQUALS = 38,
    KBD_LEFT_BRACKET = 39,
    KBD_RIGHT_BRACKET = 40,
    KBD_BACKSLASH = 41,
    KBD_SEMICOLON = 42,
    KBD_QUOTE = 43,
    KBD_BACKTICK = 44,
    KBD_COMMA = 45,
    KBD_DOT = 46,
    KBD_SLASH = 47,

    KBD_SPACE = 48,

    // Extended keypad
    KBD_KP_DIVIDE = 49,
    KBD_KP_MULTIPLY = 50,
    KBD_KP_MINUS = 51,
    KBD_KP_PLUS = 52,
    KBD_KP_PERIOD = 53,
    KBD_KP_0 = 54,
    KBD_KP_1 = 55,
    KBD_KP_2 = 56,
    KBD_KP_3 = 57,
    KBD_KP_4 = 58,
    KBD_KP_5 = 59,
    KBD_KP_6 = 60,
    KBD_KP_7 = 61,
    KBD_KP_8 = 62,
    KBD_KP_9 = 63,

    // Control keys that have ASCII codes
    KBD_KP_ENTER = 64,
    KBD_ENTER = 65,
    KBD_BACKSPACE = 66,
    KBD_TAB = 67,
    KBD_ESCAPE = 68,
    KBD_DELETE = 69,

    // Other control keys that don't have ASCII codes
    KBD_LEFT_CTRL = 70,
    KBD_RIGHT_CTRL = 71,
    KBD_LEFT_ALT = 72,
    KBD_RIGHT_ALT = 73,
    KBD_LEFT_SHIFT = 74,
    KBD_RIGHT_SHIFT = 75,
    KBD_LEFT_SUPER = 76,
    KBD_RIGHT_SUPER = 77,

    KBD_CAPSLOCK = 78,
    KBD_NUMLOCK = 79,
    KBD_SCRLLOCK = 80,

    KBD_HOME = 81,
    KBD_END = 82,
    KBD_INSERT = 83,
    KBD_PAUSE = 84,
    KBD_PRINTSCREEN = 85,

    KBD_PAGEUP = 86,
    KBD_PAGEDOWN = 87,
    KBD_LEFT = 88,
    KBD_RIGHT = 89,
    KBD_UP = 90,
    KBD_DOWN = 91,

    KBD_F1 = 92,
    KBD_F2 = 93,
    KBD_F3 = 94,
    KBD_F4 = 95,
    KBD_F5 = 96,
    KBD_F6 = 97,
    KBD_F7 = 98,
    KBD_F8 = 99,
    KBD_F9 = 100,
    KBD_F10 = 101,
    KBD_F11 = 102,
    KBD_F12 = 103,
};


enum key_action {
    KEY_UP = 0,
    KEY_DOWN = 1,
};

enum key_type_flags {
    KEY_ACTION = 1 << 7,
};

typedef struct {
    u8 source;
    u8 type;
    u8 code;
} key_event;


char kbd_to_ascii(key_event event, ascii_keymap* map, bool shift);
char kbd_to_ascii_default(key_event event);

// Convert ASCII control codes to caret notation: ^X
// https://en.wikipedia.org/wiki/Caret_notation
bool iscaret(char ch);

char ctrl_to_caret(char ascii);
char caret_to_ctrl(char ascii);
