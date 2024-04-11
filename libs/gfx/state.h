#pragma once

#include <base/types.h>

typedef enum {
    GFX_NONE,
    GFX_VESA,
    GFX_VGA,
} graphics_mode;

typedef struct {
    u8 mode;

    // If the graphics_mode is VGA these are in chars
    u16 width;
    u16 height;

    // If the graphics_mode is VGA the fields bellow are not used
    u64 framebuffer;

    u8 depth; // number of bits in every pixel
    u16 pitch; // number of bytes in each line
} graphics_state;
