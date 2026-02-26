#include "glyph.h"

#include <gui/pixel.h>

static bool term_glyph_pixel_on(
    const u8 *glyph,
    u32 glyph_row_bytes,
    u32 x,
    u32 y
) {
    const u8 *row_ptr = glyph + (size_t)y * glyph_row_bytes;
    u8 bits = row_ptr[x / 8];
    u8 mask = (u8)(0x80 >> (x & 7));
    return (bits & mask) != 0;
}

void term_glyph_blit_u32(
    u32 *dst_pixels,
    size_t dst_stride_pixels,
    const u8 *glyph,
    u32 glyph_width,
    u32 glyph_height,
    u32 glyph_row_bytes,
    u32 fg_rgb,
    u32 bg_rgb
) {
    if (
        !dst_pixels ||
        !dst_stride_pixels ||
        !glyph ||
        !glyph_width ||
        !glyph_height ||
        !glyph_row_bytes
    ) {
        return;
    }

    for (u32 gy = 0; gy < glyph_height; gy++) {
        u32 *row = dst_pixels + (size_t)gy * dst_stride_pixels;

        for (u32 gx = 0; gx < glyph_width; gx++) {
            row[gx] = term_glyph_pixel_on(glyph, glyph_row_bytes, gx, gy)
                          ? fg_rgb :
                          bg_rgb;
        }
    }
}

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
) {
    if (
        !dst_pixels ||
        !dst_pitch_bytes ||
        !fmt ||
        !fmt->bytes_per_pixel ||
        !glyph ||
        !glyph_width ||
        !glyph_height ||
        !glyph_row_bytes
    ) {
        return;
    }

    u32 fg_packed = pixel_pack_rgb888(
        fg_rgb,
        fmt->red_shift,
        fmt->green_shift,
        fmt->blue_shift,
        fmt->red_size,
        fmt->green_size,
        fmt->blue_size
    );
    u32 bg_packed = pixel_pack_rgb888(
        bg_rgb,
        fmt->red_shift,
        fmt->green_shift,
        fmt->blue_shift,
        fmt->red_size,
        fmt->green_size,
        fmt->blue_size
    );

    for (u32 gy = 0; gy < glyph_height; gy++) {
        u8 *row = dst_pixels + (size_t)gy * dst_pitch_bytes;

        for (u32 gx = 0; gx < glyph_width; gx++) {
            u32 packed =
                term_glyph_pixel_on(glyph, glyph_row_bytes, gx, gy) ?
                    fg_packed :
                    bg_packed;

            pixel_store_packed(
                row + (size_t)gx * fmt->bytes_per_pixel,
                fmt->bytes_per_pixel,
                packed
            );
        }
    }
}
