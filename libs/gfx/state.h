#pragma once

#include <base/attributes.h>
#include <base/types.h>

typedef enum {
    GFX_NONE,
    GFX_VESA,
    GFX_VGA,
} graphics_mode;

typedef struct PACKED {
    u8 mode;

    u8 depth;
    u16 pitch;

    u16 width;
    u16 height;

    u64 framebuffer;

    u8 red_mask;
    u8 green_mask;
    u8 blue_mask;
} graphics_state;
