#pragma once

#include <gui/fb.h>
#include <stdbool.h>

typedef enum {
    WM_CURSOR_NORMAL = 0,
    WM_CURSOR_RESIZE_EW = 1,
    WM_CURSOR_RESIZE_NS = 2,
    WM_CURSOR_RESIZE_NW = 3,
    WM_CURSOR_RESIZE_SE = 4,
    WM_CURSOR_RESIZE_SW = 5,
    WM_CURSOR_KIND_COUNT = 6,
} wm_cursor_kind_t;

bool wm_cursor_load(const char *path);
bool wm_cursor_load_kind(wm_cursor_kind_t kind, const char *path);
void wm_cursor_unload(void);
bool wm_cursor_draw_kind(
    pixel_t *frame,
    u32 fb_width,
    u32 fb_height,
    i32 x,
    i32 y,
    wm_cursor_kind_t kind
);
bool wm_cursor_draw(pixel_t *frame, u32 fb_width, u32 fb_height, i32 x, i32 y);
