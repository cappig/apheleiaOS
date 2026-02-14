#pragma once

#include <base/types.h>

enum input_event_type {
    INPUT_EVENT_KEY = 1,
    INPUT_EVENT_MOUSE_MOVE = 2,
    INPUT_EVENT_MOUSE_BUTTON = 3,
    INPUT_EVENT_MOUSE_WHEEL = 4,
};

enum input_modifiers {
    INPUT_MOD_SHIFT = 1 << 0,
    INPUT_MOD_CTRL = 1 << 1,
    INPUT_MOD_ALT = 1 << 2,
    INPUT_MOD_CAPS = 1 << 3,
};

typedef struct input_event {
    u64 timestamp_ms;
    u32 type;
    u32 source;
    u32 keycode;
    u32 action;
    u32 buttons;
    u32 modifiers;
    i32 dx;
    i32 dy;
    i32 wheel;
} input_event_t;
