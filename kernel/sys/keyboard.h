#pragma once

#include "input/kbd.h"

#define KBD_DEV_BUFFER_SIZE 256


bool keyboard_init(void);

void kbd_handle_key(key_event event);
