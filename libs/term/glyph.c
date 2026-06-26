#include "glyph.h"

#include <gui/pixel.h>

static bool pixel_on(const term_glyph_t *glyph, u32 x, u32 y) {
    const u8 *row_ptr = glyph->bits + (size_t)y * glyph->row_bytes;
    u8 bits = row_ptr[x / 8];
    u8 mask = (u8)(0x80 >> (x & 7));
    return (bits & mask) != 0;
}

void term_glyph_blit_u32(u32 *dst_pixels, size_t dst_stride_pixels, const term_glyph_t *glyph) {
    if (!dst_pixels || !dst_stride_pixels) {
        return;
    }

    if (!glyph || !glyph->bits || !glyph->width || !glyph->height || !glyph->row_bytes) {
        return;
    }

    for (u32 gy = 0; gy < glyph->height; gy++) {
        u32 *row = dst_pixels + (size_t)gy * dst_stride_pixels;

        for (u32 gx = 0; gx < glyph->width; gx++) {
            row[gx] = pixel_on(glyph, gx, gy) ? glyph->fg_rgb : glyph->bg_rgb;
        }
    }
}

void term_glyph_blit_packed(
    u8 *dst_pixels,
    size_t dst_pitch_bytes,
    const term_pixel_format_t *fmt,
    const term_glyph_t *glyph
) {
    bool missing_dst = !dst_pixels || !dst_pitch_bytes;
    bool missing_fmt = !fmt || !fmt->bytes_per_pixel;
    bool missing_glyph = !glyph || !glyph->bits || !glyph->width || !glyph->height || !glyph->row_bytes;

    if (missing_dst || missing_fmt || missing_glyph) {
        return;
    }

    pixel_format_t pixel_fmt = {
        .bytes_per_pixel = fmt->bytes_per_pixel,
        .red_shift = fmt->red_shift,
        .green_shift = fmt->green_shift,
        .blue_shift = fmt->blue_shift,
        .red_size = fmt->red_size,
        .green_size = fmt->green_size,
        .blue_size = fmt->blue_size,
    };

    u32 fg_packed = pixel_pack_rgb888(glyph->fg_rgb, &pixel_fmt);
    u32 bg_packed = pixel_pack_rgb888(glyph->bg_rgb, &pixel_fmt);

    for (u32 gy = 0; gy < glyph->height; gy++) {
        u8 *row = dst_pixels + (size_t)gy * dst_pitch_bytes;

        for (u32 gx = 0; gx < glyph->width; gx++) {
            u32 packed = pixel_on(glyph, gx, gy) ? fg_packed : bg_packed;

            pixel_store_packed(row + (size_t)gx * fmt->bytes_per_pixel, fmt->bytes_per_pixel, packed);
        }
    }
}
