#pragma once

#include <gui/fb.h>
#include <stdbool.h>

bool wm_background_load(u32 fb_width, u32 fb_height, const char *path);
void wm_background_unload(void);
bool wm_background_draw(pixel_t *frame, u32 fb_width, u32 fb_height);
bool wm_background_draw_rect(
    pixel_t *frame,
    u32 fb_width,
    u32 fb_height,
    i32 x,
    i32 y,
    u32 width,
    u32 height
);
