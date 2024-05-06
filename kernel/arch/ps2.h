#pragma once

#include <base/types.h>

#include "vfs/fs.h"

#define RELEASE_OFFSET 0x80
#define RELEASED(key)  (key + RELEASE_OFFSET)

// Some of these characters do have an ASCII representation but they are
// never used in text so we do this instead
enum kbd_non_ascii_keys : u8 {
    KBD_NOTHING = 0x00, // scancode doesn't have an associated key
    KBD_ESC = 0x01,
    KBD_BACKSPACE = 0x0e,
    KBD_L_CTRL = 0x1d,
    KBD_L_SHIFT = 0x2a,
    KBD_R_SHIFT = 0x36,
    KBD_ALT = 0x38,
    KBD_CAPSLOCK = 0x3a,
    KBD_F1 = 0x03b,
    KBD_F2 = 0x3c,
    KBD_F3 = 0x3d,
    KBD_F4 = 0x3e,
    KBD_F5 = 0x3f,
    KBD_F6 = 0x40,
    KBD_F7 = 0x41,
    KBD_F8 = 0x42,
    KBD_F9 = 0x43,
    KBD_F10 = 0x44,
    KBD_NUMLOCK = 0x45,
    KBD_SCROLLLOCK = 0x46,
    KBD_F11 = 0x57,
    KBD_F12 = 0x58,
};

void init_ps2_kbd(virtual_fs* vfs);
