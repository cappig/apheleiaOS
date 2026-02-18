#pragma once

#include <base/types.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    u32 width;
    u32 height;
    const u8* raster;
    size_t raster_size;
} ppm_p6_blob_t;

bool ppm_parse_p6_blob(const void* data, size_t size, ppm_p6_blob_t* out);
