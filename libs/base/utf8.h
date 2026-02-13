#pragma once

#include <base/types.h>

static inline size_t utf8_sequence_len(u8 lead) {
    if (lead < 0x80)
        return 1;
    if ((lead & 0xe0) == 0xc0)
        return 2;
    if ((lead & 0xf0) == 0xe0)
        return 3;
    if ((lead & 0xf8) == 0xf0)
        return 4;

    return 0;
}

static inline size_t utf8_decode(const u8* data, size_t len, u32* out) {
    if (!data || !len || !out)
        return 0;

    u8 b0 = data[0];
    if (b0 < 0x80) {
        *out = b0;
        return 1;
    }

    if ((b0 & 0xe0) == 0xc0) {
        if (len < 2 || (data[1] & 0xc0) != 0x80)
            return 0;

        u32 cp = ((u32)(b0 & 0x1f) << 6) | (u32)(data[1] & 0x3f);
        if (cp < 0x80)
            return 0;

        *out = cp;
        return 2;
    }

    if ((b0 & 0xf0) == 0xe0) {
        if (len < 3 || (data[1] & 0xc0) != 0x80 || (data[2] & 0xc0) != 0x80)
            return 0;

        u32 cp = ((u32)(b0 & 0x0f) << 12) | ((u32)(data[1] & 0x3f) << 6) | (u32)(data[2] & 0x3f);
        if (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))
            return 0;

        *out = cp;
        return 3;
    }

    if ((b0 & 0xf8) == 0xf0) {
        if (len < 4 || (data[1] & 0xc0) != 0x80 || (data[2] & 0xc0) != 0x80 ||
            (data[3] & 0xc0) != 0x80)
            return 0;

        u32 cp = ((u32)(b0 & 0x07) << 18) | ((u32)(data[1] & 0x3f) << 12) |
                 ((u32)(data[2] & 0x3f) << 6) | (u32)(data[3] & 0x3f);
        if (cp < 0x10000 || cp > 0x10ffff)
            return 0;

        *out = cp;
        return 4;
    }

    return 0;
}
