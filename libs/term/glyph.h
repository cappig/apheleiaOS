#pragma once

#include <base/types.h>
#include <stddef.h>

typedef struct {
    u8 bytes_per_pixel;
    u8 red_shift;
    u8 green_shift;
    u8 blue_shift;
    u8 red_size;
    u8 green_size;
    u8 blue_size;
} term_pixel_format_t;

#define TERM_GLYPH_CELL_GAP_PX 1U

void term_glyph_blit_u32(
    u32 *dst_pixels,
    size_t dst_stride_pixels,
    const u8 *glyph,
    u32 glyph_width,
    u32 glyph_height,
    u32 glyph_row_bytes,
    u32 fg_rgb,
    u32 bg_rgb
);

void term_glyph_blit_packed(
    u8 *dst_pixels,
    size_t dst_pitch_bytes,
    const term_pixel_format_t *fmt,
    const u8 *glyph,
    u32 glyph_width,
    u32 glyph_height,
    u32 glyph_row_bytes,
    u32 fg_rgb,
    u32 bg_rgb
);
