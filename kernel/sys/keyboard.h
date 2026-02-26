#pragma once

#include <base/types.h>
#include <input/kbd.h>
#include <input/keymap.h>

bool keyboard_init(void);
u8 keyboard_register(const char *name, ascii_keymap *keymap);
void keyboard_handle_key(key_event event);
