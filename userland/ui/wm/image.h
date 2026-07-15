#pragma once

#include <base/types.h>
#include <gui/fb.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    u32 width;
    u32 height;
    pixel_t *pixels;
} wm_image_t;

bool wm_image_load(const char *path, size_t max_bytes, size_t max_pixels, wm_image_t *out);
void wm_image_release(wm_image_t *image);
