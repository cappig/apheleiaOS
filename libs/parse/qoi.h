#pragma once

#include <base/types.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    u32 width;
    u32 height;
    u8 channels;
    u8 colorspace;
} qoi_info_t;

bool qoi_read_info(const void *data, size_t size, qoi_info_t *out);
/* Pixels are decoded as 0xAARRGGBB. */
bool qoi_decode(const void *data, size_t size, u32 *pixels, size_t count);
