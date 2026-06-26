#pragma once

#include <base/types.h>
#include <stdbool.h>

typedef struct {
    u8 bytes_per_pixel;
    u8 red_shift;
    u8 green_shift;
    u8 blue_shift;
    u8 red_size;
    u8 green_size;
    u8 blue_size;
} pixel_format_t;

static inline u32 pixel_channel_mask(u8 bits) {
    if (!bits)
        return 0;

    if (bits >= 32)
        return 0xffffffffU;

    return (1U << bits) - 1U;
}

static inline u32 pixel_scale_u8(u8 value, u8 bits) {
    if (!bits)
        return 0;

    if (bits >= 8)
        return (u32)value;

    u32 max = pixel_channel_mask(bits);
    return ((u32)value * max + 127U) / 255U;
}

static inline void pixel_fill_rgb_defaults(pixel_format_t *fmt) {
    if (!fmt)
        return;

    bool already_set = fmt->red_size && fmt->green_size && fmt->blue_size;
    if (already_set)
        return;

    switch (fmt->bytes_per_pixel) {
    case 2:
        fmt->red_shift = 11;
        fmt->green_shift = 5;
        fmt->blue_shift = 0;
        fmt->red_size = 5;
        fmt->green_size = 6;
        fmt->blue_size = 5;
        break;
    case 3:
    case 4:
    default:
        fmt->red_shift = 16;
        fmt->green_shift = 8;
        fmt->blue_shift = 0;
        fmt->red_size = 8;
        fmt->green_size = 8;
        fmt->blue_size = 8;
        break;
    }
}

static inline bool pixel_is_fast_bgrx8888(const pixel_format_t *fmt) {
    if (!fmt || fmt->bytes_per_pixel != 4) {
        return false;
    }

    bool shifts_match = fmt->red_shift == 16 && fmt->green_shift == 8 && fmt->blue_shift == 0;
    bool sizes_match = fmt->red_size == 8 && fmt->green_size == 8 && fmt->blue_size == 8;

    return shifts_match && sizes_match;
}

static inline u32 pixel_pack_rgb888(u32 color, const pixel_format_t *fmt) {
    if (!fmt) {
        return 0;
    }

    u8 r8 = (u8)((color >> 16) & 0xffU);
    u8 g8 = (u8)((color >> 8) & 0xffU);
    u8 b8 = (u8)(color & 0xffU);

    u32 out = 0;

    if (fmt->red_size && fmt->red_shift < 32) {
        u32 r = pixel_scale_u8(r8, fmt->red_size) & pixel_channel_mask(fmt->red_size);
        out |= (r << fmt->red_shift);
    }

    if (fmt->green_size && fmt->green_shift < 32) {
        u32 g = pixel_scale_u8(g8, fmt->green_size) & pixel_channel_mask(fmt->green_size);
        out |= (g << fmt->green_shift);
    }

    if (fmt->blue_size && fmt->blue_shift < 32) {
        u32 b = pixel_scale_u8(b8, fmt->blue_size) & pixel_channel_mask(fmt->blue_size);
        out |= (b << fmt->blue_shift);
    }

    return out;
}

static inline void pixel_store_packed(void *dst, u8 bytes_per_pixel, u32 packed) {
    if (!dst)
        return;

    u8 *out = dst;

    switch (bytes_per_pixel) {
    case 4:
        *(u32 *)out = packed;
        break;
    case 3:
        out[0] = (u8)(packed & 0xffU);
        out[1] = (u8)((packed >> 8) & 0xffU);
        out[2] = (u8)((packed >> 16) & 0xffU);
        break;
    case 2:
        *(u16 *)out = (u16)(packed & 0xffffU);
        break;
    case 1:
        out[0] = (u8)(packed & 0xffU);
        break;
    default:
        break;
    }
}
