#pragma once

#include <base/types.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    PSF_TYPE_NONE = 0,
    PSF_TYPE_1 = 1,
    PSF_TYPE_2 = 2,
} psf_type_t;

enum psf_blob_flags {
    PSF_BLOB_UNICODE = 1U << 0,
};

typedef struct {
    psf_type_t type;
    u32 flags;
    u32 width;
    u32 height;
    u32 glyph_count;
    u32 glyph_size;
    u32 row_bytes;
    const u8* glyphs;
    const u8* unicode_table;
    size_t unicode_size;
} psf_blob_t;

bool psf_parse_blob(const void* data, size_t size, psf_blob_t* out);
