#pragma once

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
} graphics_state;
