#pragma once

#include <base/types.h>
#include <input/mouse.h>

#define MOUSE_DEV_BUFFER_SIZE 256

typedef struct {
    const char* name;
} mouse;


void mouse_handle_event(mouse_event event);

u8 register_mouse(char* name);

bool mouse_init(void);
