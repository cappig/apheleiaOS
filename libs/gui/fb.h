#pragma once

#include <base/types.h>
#include <stdbool.h>

typedef struct fb_info {
    u32 width;
    u32 height;
    u32 pitch;
    u8 bpp;
    bool available;
} fb_info_t;

typedef struct fb_present_rect {
    const u32 *frame;
    u32 x;
    u32 y;
    u32 width;
    u32 height;
} fb_present_rect_t;
