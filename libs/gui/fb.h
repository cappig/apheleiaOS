#pragma once

#include <base/types.h>
#include <stdbool.h>
#include <stddef.h>

typedef u32 pixel_t;

typedef struct framebuffer {
    pixel_t *pixels;
    u32 width;
    u32 height;
    u32 stride;
    size_t pixel_count;
} framebuffer_t;

typedef struct fb_info {
    u32 width;
    u32 height;
    u32 pitch;
    u8 bpp;
    bool available;
} fb_info_t;

typedef struct fb_present_rect {
    const pixel_t *frame;
    u32 x;
    u32 y;
    u32 width;
    u32 height;
} fb_present_rect_t;
