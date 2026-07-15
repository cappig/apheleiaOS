#include "background.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "color.h"
#include "image.h"

#define BG_FILE_MAX  (64U * 1024U * 1024U)
#define BG_PIXEL_MAX (16U * 1024U * 1024U)

typedef struct {
    pixel_t *pixels;
    u32 width;
    u32 height;
} wm_background_t;

static wm_background_t background = { 0 };
static const size_t k_size_max = (size_t)-1;

static void _fill_solid_color(size_t pixels, pixel_t color) {
    for (size_t i = 0; i < pixels; i++) {
        background.pixels[i] = color;
    }
}

static void _build_cover_map(u32 src_w, u32 src_h, u32 dst_w, u32 dst_h, u32 *x_map, u32 *y_map) {
    u32 crop_x = 0;
    u32 crop_y = 0;
    u32 crop_w = src_w;
    u32 crop_h = src_h;

    u64 lhs = (u64)src_w * (u64)dst_h;
    u64 rhs = (u64)src_h * (u64)dst_w;

    if (lhs > rhs) {
        crop_w = (u32)(((u64)src_h * (u64)dst_w) / (u64)dst_h);
        if (!crop_w) {
            crop_w = 1;
        }

        crop_x = (src_w - crop_w) / 2;
    } else if (lhs < rhs) {
        crop_h = (u32)(((u64)src_w * (u64)dst_h) / (u64)dst_w);
        if (!crop_h) {
            crop_h = 1;
        }

        crop_y = (src_h - crop_h) / 2;
    }

    for (u32 x = 0; x < dst_w; x++) {
        u64 num = (u64)(2U * x + 1U) * (u64)crop_w;
        u32 sx = crop_x + (u32)(num / (2ULL * (u64)dst_w));
        if (sx >= src_w) {
            sx = src_w - 1;
        }

        x_map[x] = sx;
    }

    for (u32 y = 0; y < dst_h; y++) {
        u64 num = (u64)(2U * y + 1U) * (u64)crop_h;
        u32 sy = crop_y + (u32)(num / (2ULL * (u64)dst_h));
        if (sy >= src_h) {
            sy = src_h - 1;
        }

        y_map[y] = sy;
    }
}

void wm_background_unload(void) {
    if (background.pixels) {
        free(background.pixels);
    }

    memset(&background, 0, sizeof(background));
}

bool wm_background_load(u32 fb_width, u32 fb_height, const char *path) {
    wm_background_unload();

    if (!path || !path[0] || !fb_width || !fb_height) {
        return false;
    }

    size_t dst_pixels = (size_t)fb_width * (size_t)fb_height;
    if (fb_height && dst_pixels / fb_height != fb_width) {
        return false;
    }

    if (dst_pixels > k_size_max / sizeof(pixel_t)) {
        return false;
    }

    pixel_t *dst = malloc(dst_pixels * sizeof(pixel_t));
    if (!dst) {
        return false;
    }

    background.pixels = dst;

    u32 solid_color = 0;
    if (wm_parse_hex_color(path, &solid_color)) {
        _fill_solid_color(dst_pixels, solid_color);
        background.width = fb_width;
        background.height = fb_height;
        return true;
    }

    wm_image_t source = { 0 };
    if (!wm_image_load(path, BG_FILE_MAX, BG_PIXEL_MAX, &source)) {
        goto fail;
    }

    u32 *x_map = malloc((size_t)fb_width * sizeof(u32));
    u32 *y_map = malloc((size_t)fb_height * sizeof(u32));
    if (!x_map || !y_map) {
        if (x_map) {
            free(x_map);
        }
        if (y_map) {
            free(y_map);
        }
        goto fail;
    }

    u32 src_w = source.width;
    u32 src_h = source.height;
    _build_cover_map(src_w, src_h, fb_width, fb_height, x_map, y_map);

    u32 prev_src_y = 0;
    bool have_prev_row = false;

    for (u32 y = 0; y < fb_height; y++) {
        size_t dst_row = (size_t)y * fb_width;
        u32 src_y = y_map[y];

        if (have_prev_row && src_y == prev_src_y) {
            memcpy(dst + dst_row, dst + dst_row - fb_width, (size_t)fb_width * sizeof(*dst));
            continue;
        }

        size_t src_row = (size_t)src_y * (size_t)src_w;

        for (u32 x = 0; x < fb_width; x++) {
            dst[dst_row + x] = source.pixels[src_row + x_map[x]];
        }

        prev_src_y = src_y;
        have_prev_row = true;
    }

    free(x_map);
    free(y_map);
    wm_image_release(&source);

    background.width = fb_width;
    background.height = fb_height;
    return true;

fail:
    wm_image_release(&source);
    wm_background_unload();
    return false;
}

bool wm_background_draw(pixel_t *frame, u32 fb_width, u32 fb_height) {
    bool size_ok = background.width == fb_width && background.height == fb_height;
    if (!frame || !background.pixels || !size_ok) {
        return false;
    }

    size_t pixels = (size_t)fb_width * (size_t)fb_height;
    if (fb_height && pixels / fb_height != fb_width) {
        return false;
    }

    if (pixels > k_size_max / sizeof(pixel_t)) {
        return false;
    }

    memcpy(frame, background.pixels, pixels * sizeof(pixel_t));
    return true;
}

bool wm_background_draw_rect(pixel_t *frame, u32 fb_width, u32 fb_height, const wm_rect_t *rect) {
    bool missing_frame = !frame || !background.pixels;
    bool wrong_size = background.width != fb_width || background.height != fb_height;
    bool empty_rect = !rect || !rect->width || !rect->height;

    if (missing_frame || wrong_size || empty_rect) {
        return false;
    }

    i32 x0 = rect->x;
    i32 y0 = rect->y;
    i32 x1 = rect->x + rect->width;
    i32 y1 = rect->y + rect->height;

    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > (i32)fb_width) {
        x1 = (i32)fb_width;
    }
    if (y1 > (i32)fb_height) {
        y1 = (i32)fb_height;
    }

    if (x0 >= x1 || y0 >= y1) {
        return true;
    }

    size_t row_pixels = (size_t)(x1 - x0);
    size_t row_bytes = row_pixels * sizeof(pixel_t);

    for (i32 row = y0; row < y1; row++) {
        size_t off = (size_t)row * fb_width + (size_t)x0;
        memcpy(frame + off, background.pixels + off, row_bytes);
    }

    return true;
}
