#include "wm_cursor.h"

#include <parse/ppm.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "wm_file.h"

#define WM_CURSOR_MAX_FILE_BYTES (1U * 1024U * 1024U)
#define WM_CURSOR_WIDTH          16U
#define WM_CURSOR_HEIGHT         16U
#define WM_CURSOR_KEY_COLOR      0x00ff00ffU

typedef struct {
    u32 width;
    u32 height;
    u32 *pixels;
} wm_cursor_t;

static wm_cursor_t cursor = {0};


void wm_cursor_unload(void) {
    if (cursor.pixels) {
        free(cursor.pixels);
    }

    memset(&cursor, 0, sizeof(cursor));
}

bool wm_cursor_load(const char *path) {
    wm_cursor_unload();

    if (!path || !path[0]) {
        return false;
    }

    u8 *file_data = NULL;
    size_t file_len = 0;

    if (!wm_file_read_all(path, WM_CURSOR_MAX_FILE_BYTES, &file_data, &file_len)) {
        return false;
    }

    ppm_p6_blob_t ppm_blob = {0};
    if (!ppm_parse_p6_blob(file_data, file_len, &ppm_blob)) {
        free(file_data);
        return false;
    }

    u32 width = ppm_blob.width;
    u32 height = ppm_blob.height;

    if (width != WM_CURSOR_WIDTH || height != WM_CURSOR_HEIGHT) {
        free(file_data);
        return false;
    }

    size_t pixel_count = (size_t)width * (size_t)height;
    if (height && pixel_count / height != width) {
        free(file_data);
        return false;
    }

    size_t size_max = (size_t)-1;
    if (pixel_count > size_max / sizeof(u32)) {
        free(file_data);
        return false;
    }

    u32 *pixels = malloc(pixel_count * sizeof(u32));
    if (!pixels) {
        free(file_data);
        return false;
    }

    const u8 *raster = ppm_blob.raster;
    for (size_t i = 0; i < pixel_count; i++) {
        u8 r = raster[i * 3 + 0];
        u8 g = raster[i * 3 + 1];
        u8 b = raster[i * 3 + 2];
        u32 color = ((u32)r << 16) | ((u32)g << 8) | (u32)b;

        pixels[i] = color;
    }

    free(file_data);

    cursor.width = width;
    cursor.height = height;
    cursor.pixels = pixels;

    return true;
}

bool wm_cursor_draw(u32 *frame, u32 fb_width, u32 fb_height, i32 x, i32 y) {
    if (!frame || !cursor.pixels || !cursor.width || !cursor.height || !fb_width || !fb_height) {
        return false;
    }

    for (u32 cy = 0; cy < cursor.height; cy++) {
        i32 dst_y = y + (i32)cy;
        if (dst_y < 0 || (u32)dst_y >= fb_height) {
            continue;
        }

        size_t src_row = (size_t)cy * (size_t)cursor.width;
        size_t dst_row = (size_t)dst_y * (size_t)fb_width;

        for (u32 cx = 0; cx < cursor.width; cx++) {
            u32 color = cursor.pixels[src_row + cx];
            if (color == WM_CURSOR_KEY_COLOR) {
                continue;
            }

            i32 dst_x = x + (i32)cx;
            if (dst_x < 0 || (u32)dst_x >= fb_width) {
                continue;
            }

            frame[dst_row + (size_t)dst_x] = color;
        }
    }

    return true;
}
