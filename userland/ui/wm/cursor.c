#include "cursor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "image.h"

#define CURSOR_MAX_BYTES (1U * 1024U * 1024U)
#define CURSOR_WIDTH     16U
#define CURSOR_HEIGHT    16U
#define CURSOR_KEY_COLOR 0x00ff00ffU

typedef struct {
    u32 width;
    u32 height;
    pixel_t *pixels;
} wm_cursor_t;

static wm_cursor_t cursors[WM_CURSOR_KIND_COUNT];

static void _cursor_release(wm_cursor_t *cursor) {
    if (!cursor) {
        return;
    }

    if (cursor->pixels) {
        free(cursor->pixels);
    }

    memset(cursor, 0, sizeof(*cursor));
}

static bool _cursor_load_into(wm_cursor_t *cursor, const char *path) {
    if (!cursor) {
        return false;
    }

    _cursor_release(cursor);

    if (!path || !path[0]) {
        return false;
    }

    wm_image_t image = { 0 };
    if (!wm_image_load(path, CURSOR_MAX_BYTES, CURSOR_WIDTH * CURSOR_HEIGHT, &image)) {
        return false;
    }

    if (image.width != CURSOR_WIDTH || image.height != CURSOR_HEIGHT) {
        wm_image_release(&image);
        return false;
    }

    cursor->width = image.width;
    cursor->height = image.height;
    cursor->pixels = image.pixels;
    image.pixels = NULL;
    wm_image_release(&image);

    return true;
}

static const wm_cursor_t *_cursor_pick(wm_cursor_kind_t kind) {
    if (kind < WM_CURSOR_KIND_COUNT && cursors[kind].pixels) {
        return &cursors[kind];
    }

    if (cursors[WM_CURSOR_NORMAL].pixels) {
        return &cursors[WM_CURSOR_NORMAL];
    }

    return NULL;
}

static bool _center_hotspot(wm_cursor_kind_t kind) {
    switch (kind) {
    case WM_CURSOR_RESIZE_EW:
    case WM_CURSOR_RESIZE_NS:
    case WM_CURSOR_RESIZE_NW:
    case WM_CURSOR_RESIZE_SE:
    case WM_CURSOR_RESIZE_SW:
    case WM_CURSOR_MOVE:
        return true;
    default:
        return false;
    }
}

void wm_cursor_unload(void) {
    for (u32 i = 0; i < WM_CURSOR_KIND_COUNT; i++) {
        _cursor_release(&cursors[i]);
    }
}

bool wm_cursor_load_kind(wm_cursor_kind_t kind, const char *path) {
    if (kind >= WM_CURSOR_KIND_COUNT) {
        return false;
    }

    return _cursor_load_into(&cursors[kind], path);
}

bool wm_cursor_draw_kind(pixel_t *frame, u32 fb_width, u32 fb_height, i32 x, i32 y, wm_cursor_kind_t kind) {
    const wm_cursor_t *cursor = _cursor_pick(kind);

    bool valid_fb = fb_width && fb_height;

    if (!frame || !cursor || !cursor->pixels || !valid_fb) {
        return false;
    }

    if (!cursor->width || !cursor->height) {
        return false;
    }

    bool has_exact_cursor = kind < WM_CURSOR_KIND_COUNT && cursors[kind].pixels != NULL;

    i32 hot_x = 0;
    i32 hot_y = 0;

    if (has_exact_cursor && _center_hotspot(kind)) {
        hot_x = (i32)(cursor->width / 2);
        hot_y = (i32)(cursor->height / 2);
    }

    for (u32 cy = 0; cy < cursor->height; cy++) {
        i32 dst_y = (y - hot_y) + (i32)cy;

        if (dst_y < 0 || (u32)dst_y >= fb_height) {
            continue;
        }

        size_t src_row = (size_t)cy * (size_t)cursor->width;
        size_t dst_row = (size_t)dst_y * (size_t)fb_width;

        for (u32 cx = 0; cx < cursor->width; cx++) {
            u32 color = cursor->pixels[src_row + cx];
            if (color == CURSOR_KEY_COLOR) {
                continue;
            }

            i32 dst_x = (x - hot_x) + (i32)cx;
            if (dst_x < 0 || (u32)dst_x >= fb_width) {
                continue;
            }

            frame[dst_row + (size_t)dst_x] = color;
        }
    }

    return true;
}
