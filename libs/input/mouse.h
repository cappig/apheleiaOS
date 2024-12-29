#pragma once

#include <base/types.h>


typedef enum {
    MOUSE_RIGHT_CLICK = 1 << 0,
    MOUSE_LEFT_CLICK = 1 << 1,
    MOUSE_MIDDLE_CLICK = 1 << 2,
} mouse_buttons;

typedef struct {
    // These values are relative to the preveous position
    // Coordinates are in quadrant 4 i.e. y grows down and x grows right
    i16 delta_x;
    i16 delta_y;

    u8 buttons;
    u8 source;
} mouse_event;
