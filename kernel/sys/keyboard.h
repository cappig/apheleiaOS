#pragma once

#include <input/kbd.h>
#include <input/keymap.h>

#define KBD_DEV_BUFFER_SIZE 256

typedef struct {
    const char* name;

    bool shift;
    bool ctrl;
    bool alt;
    bool capslock;

    ascii_keymap* keymap;
} keyboard;


bool keyboard_init(void);

u8 register_keyboard(char* name, ascii_keymap* keymap);

void kbd_handle_key(key_event event);
