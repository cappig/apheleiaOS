#pragma once

#include <base/types.h>
#include <stdbool.h>

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

static inline void pixel_apply_legacy_defaults(
    u8 bytes_per_pixel,
    u8* red_shift,
    u8* green_shift,
    u8* blue_shift,
    u8* red_size,
    u8* green_size,
    u8* blue_size
) {
    if (!red_shift || !green_shift || !blue_shift || !red_size || !green_size || !blue_size)
        return;

    if (*red_size && *green_size && *blue_size)
        return;

    switch (bytes_per_pixel) {
    case 2:
        *red_shift = 11;
        *green_shift = 5;
        *blue_shift = 0;
        *red_size = 5;
        *green_size = 6;
        *blue_size = 5;
        break;
    case 3:
    case 4:
    default:
        *red_shift = 16;
        *green_shift = 8;
        *blue_shift = 0;
        *red_size = 8;
        *green_size = 8;
        *blue_size = 8;
        break;
    }
}

static inline bool pixel_is_fast_bgrx8888(
    u8 bytes_per_pixel,
    u8 red_shift,
    u8 green_shift,
    u8 blue_shift,
    u8 red_size,
    u8 green_size,
    u8 blue_size
) {
    return bytes_per_pixel == 4 && red_shift == 16 && green_shift == 8 && blue_shift == 0 &&
           red_size == 8 && green_size == 8 && blue_size == 8;
}

static inline u32 pixel_pack_rgb888(
    u32 color,
    u8 red_shift,
    u8 green_shift,
    u8 blue_shift,
    u8 red_size,
    u8 green_size,
    u8 blue_size
) {
    u8 r8 = (u8)((color >> 16) & 0xffU);
    u8 g8 = (u8)((color >> 8) & 0xffU);
    u8 b8 = (u8)(color & 0xffU);

    u32 out = 0;

    if (red_size && red_shift < 32) {
        u32 r = pixel_scale_u8(r8, red_size) & pixel_channel_mask(red_size);
        out |= (r << red_shift);
    }

    if (green_size && green_shift < 32) {
        u32 g = pixel_scale_u8(g8, green_size) & pixel_channel_mask(green_size);
        out |= (g << green_shift);
    }

    if (blue_size && blue_shift < 32) {
        u32 b = pixel_scale_u8(b8, blue_size) & pixel_channel_mask(blue_size);
        out |= (b << blue_shift);
    }

    return out;
}

static inline void pixel_store_packed(void* dst, u8 bytes_per_pixel, u32 packed) {
    if (!dst)
        return;

    u8* out = dst;

    switch (bytes_per_pixel) {
    case 4:
        *(u32*)out = packed;
        break;
    case 3:
        out[0] = (u8)(packed & 0xffU);
        out[1] = (u8)((packed >> 8) & 0xffU);
        out[2] = (u8)((packed >> 16) & 0xffU);
        break;
    case 2:
        *(u16*)out = (u16)(packed & 0xffffU);
        break;
    case 1:
        out[0] = (u8)(packed & 0xffU);
        break;
    default:
        break;
    }
}
