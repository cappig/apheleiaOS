#include "wm_background.h"

#include <limits.h>
#include <parse/ppm.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "wm_file.h"

#define WM_BG_MAX_FILE_BYTES (64U * 1024U * 1024U)

static pixel_t *bg_pixels = NULL;
static u32 bg_width = 0;
static u32 bg_height = 0;

static int _hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }

    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }

    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }

    return -1;
}

static bool _parse_hex_color(const char *text, u32 *color_out) {
    if (!text || !color_out) {
        return false;
    }

    const char *pos = text;
    if (pos[0] == '#') {
        pos++;
    } else if (pos[0] == '0' && (pos[1] == 'x' || pos[1] == 'X')) {
        pos += 2;
    }

    u32 value = 0;
    size_t digits = 0;
    while (digits < 8) {
        int nib = _hex_nibble(pos[digits]);
        if (nib < 0) {
            break;
        }

        value = (value << 4) | (u32)nib;
        digits++;
    }

    if (pos[digits] != '\0') {
        return false;
    }

    if (digits == 6) {
        *color_out = value;
        return true;
    }

    if (digits == 8) {
        *color_out = value & 0x00ffffffU;
        return true;
    }

    return false;
}

static void _fill_solid_color(size_t pixels, pixel_t color) {
    for (size_t i = 0; i < pixels; i++) {
        bg_pixels[i] = color;
    }
}

static void _build_cover_map(
    u32 src_w,
    u32 src_h,
    u32 dst_w,
    u32 dst_h,
    u32 *x_map,
    u32 *y_map
) {
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
    if (bg_pixels) {
        free(bg_pixels);
    }

    bg_pixels = NULL;
    bg_width = 0;
    bg_height = 0;
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

    if (dst_pixels > SIZE_MAX / sizeof(pixel_t)) {
        return false;
    }

    pixel_t *dst = malloc(dst_pixels * sizeof(pixel_t));
    if (!dst) {
        return false;
    }

    bg_pixels = dst;

    u32 solid_color = 0;
    if (_parse_hex_color(path, &solid_color)) {
        _fill_solid_color(dst_pixels, solid_color);
        bg_width = fb_width;
        bg_height = fb_height;
        return true;
    }

    u8 *file_data = NULL;
    size_t file_len = 0;

    if (!wm_file_read_all(path, WM_BG_MAX_FILE_BYTES, &file_data, &file_len)) {
        goto fail;
    }

    ppm_p6_blob_t ppm_blob = {0};
    if (!ppm_parse_p6_blob(file_data, file_len, &ppm_blob)) {
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

    const u8 *raster = ppm_blob.raster;
    u32 src_w = ppm_blob.width;
    u32 src_h = ppm_blob.height;

    _build_cover_map(src_w, src_h, fb_width, fb_height, x_map, y_map);

    for (u32 y = 0; y < fb_height; y++) {
        size_t dst_row = (size_t)y * fb_width;
        size_t src_row = (size_t)y_map[y] * (size_t)src_w * 3;

        for (u32 x = 0; x < fb_width; x++) {
            size_t src_off = src_row + (size_t)x_map[x] * 3;
            u8 r = raster[src_off + 0];
            u8 g = raster[src_off + 1];
            u8 b = raster[src_off + 2];
            dst[dst_row + x] = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }
    }

    free(x_map);
    free(y_map);
    free(file_data);

    bg_width = fb_width;
    bg_height = fb_height;
    return true;

fail:
    if (file_data) {
        free(file_data);
    }

    wm_background_unload();
    return false;
}

bool wm_background_draw(pixel_t *frame, u32 fb_width, u32 fb_height) {
    if (!frame || !bg_pixels || bg_width != fb_width || bg_height != fb_height) {
        return false;
    }

    size_t pixels = (size_t)fb_width * (size_t)fb_height;
    if (fb_height && pixels / fb_height != fb_width) {
        return false;
    }

    if (pixels > SIZE_MAX / sizeof(pixel_t)) {
        return false;
    }

    memcpy(frame, bg_pixels, pixels * sizeof(pixel_t));
    return true;
}

bool wm_background_draw_rect(
    pixel_t *frame,
    u32 fb_width,
    u32 fb_height,
    i32 x,
    i32 y,
    u32 width,
    u32 height
) {
    if (
        !frame ||
        !bg_pixels ||
        bg_width != fb_width ||
        bg_height != fb_height ||
        !width ||
        !height
    ) {
        return false;
    }

    i32 x0 = x;
    i32 y0 = y;
    i32 x1 = x + (i32)width;
    i32 y1 = y + (i32)height;

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
        memcpy(frame + off, bg_pixels + off, row_bytes);
    }

    return true;
}
