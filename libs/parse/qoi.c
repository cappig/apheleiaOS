#include "qoi.h"

#include <string.h>

#define QOI_HEADER_SIZE 14U
#define QOI_END_SIZE    8U
#define QOI_OP_RGB      0xfeU
#define QOI_OP_RGBA     0xffU
#define QOI_OP_INDEX    0x00U
#define QOI_OP_DIFF     0x40U
#define QOI_OP_LUMA     0x80U
#define QOI_OP_RUN      0xc0U
#define QOI_OP_MASK     0xc0U

typedef struct {
    u8 r;
    u8 g;
    u8 b;
    u8 a;
} qoi_pixel_t;

static const u8 qoi_end[QOI_END_SIZE] = { 0, 0, 0, 0, 0, 0, 0, 1 };

static u32 _read_u32(const u8 *data) {
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) | ((u32)data[2] << 8) | (u32)data[3];
}

static u8 _hash(qoi_pixel_t pixel) {
    return (u8)((pixel.r * 3U + pixel.g * 5U + pixel.b * 7U + pixel.a * 11U) & 63U);
}

static u32 _pack(qoi_pixel_t pixel) {
    return ((u32)pixel.a << 24) | ((u32)pixel.r << 16) | ((u32)pixel.g << 8) | (u32)pixel.b;
}

bool qoi_read_info(const void *data, size_t size, qoi_info_t *out) {
    if (!out) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    if (!data || size < QOI_HEADER_SIZE + QOI_END_SIZE) {
        return false;
    }

    const u8 *bytes = data;
    if (memcmp(bytes, "qoif", 4) != 0 || memcmp(bytes + size - QOI_END_SIZE, qoi_end, QOI_END_SIZE) != 0) {
        return false;
    }

    u32 width = _read_u32(bytes + 4);
    u32 height = _read_u32(bytes + 8);
    u8 channels = bytes[12];
    u8 colorspace = bytes[13];

    if (!width || !height || (channels != 3 && channels != 4) || colorspace > 1) {
        return false;
    }

    size_t count = (size_t)width * (size_t)height;
    if (count / height != width) {
        return false;
    }

    out->width = width;
    out->height = height;
    out->channels = channels;
    out->colorspace = colorspace;
    return true;
}

bool qoi_decode(const void *data, size_t size, u32 *pixels, size_t count) {
    qoi_info_t info = { 0 };
    if (!pixels || !qoi_read_info(data, size, &info)) {
        return false;
    }

    size_t total = (size_t)info.width * (size_t)info.height;
    if (count < total) {
        return false;
    }

    const u8 *bytes = data;
    size_t pos = QOI_HEADER_SIZE;
    size_t end = size - QOI_END_SIZE;
    qoi_pixel_t index[64] = { 0 };
    qoi_pixel_t pixel = { .r = 0, .g = 0, .b = 0, .a = 255 };

    size_t out = 0;
    while (out < total) {
        if (pos >= end) {
            return false;
        }

        u8 byte = bytes[pos++];
        size_t run = 1;

        if (byte == QOI_OP_RGB) {
            if (end - pos < 3) {
                return false;
            }

            pixel.r = bytes[pos++];
            pixel.g = bytes[pos++];
            pixel.b = bytes[pos++];
        } else if (byte == QOI_OP_RGBA) {
            if (end - pos < 4) {
                return false;
            }

            pixel.r = bytes[pos++];
            pixel.g = bytes[pos++];
            pixel.b = bytes[pos++];
            pixel.a = bytes[pos++];
        } else {
            switch (byte & QOI_OP_MASK) {
            case QOI_OP_INDEX:
                pixel = index[byte & 0x3f];
                break;
            case QOI_OP_DIFF:
                pixel.r = (u8)(pixel.r + ((byte >> 4) & 0x03) - 2);
                pixel.g = (u8)(pixel.g + ((byte >> 2) & 0x03) - 2);
                pixel.b = (u8)(pixel.b + (byte & 0x03) - 2);
                break;
            case QOI_OP_LUMA: {
                if (pos >= end) {
                    return false;
                }

                i32 dg = (i32)(byte & 0x3f) - 32;
                u8 diff = bytes[pos++];
                pixel.r = (u8)(pixel.r + dg + (i32)(diff >> 4) - 8);
                pixel.g = (u8)(pixel.g + dg);
                pixel.b = (u8)(pixel.b + dg + (i32)(diff & 0x0f) - 8);
                break;
            }
            case QOI_OP_RUN:
                run = (size_t)(byte & 0x3f) + 1;
                break;
            }
        }

        if (run > total - out) {
            return false;
        }

        index[_hash(pixel)] = pixel;

        u32 color = _pack(pixel);
        for (size_t i = 0; i < run; i++) {
            pixels[out++] = color;
        }
    }

    return pos == end;
}
