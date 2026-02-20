#include "ppm.h"

#include <ctype.h>
#include <limits.h>
#include <string.h>

typedef struct {
    const u8 *data;
    size_t size;
    size_t pos;
} ppm_reader_t;

static void _skip_ws_and_comments(ppm_reader_t *reader) {
    if (!reader) {
        return;
    }

    while (reader->pos < reader->size) {
        u8 ch = reader->data[reader->pos];

        if (isspace((unsigned char)ch)) {
            reader->pos++;
            continue;
        }

        if (ch != '#') {
            break;
        }

        while (reader->pos < reader->size && reader->data[reader->pos] != '\n') {
            reader->pos++;
        }
    }
}

static bool _parse_u32(ppm_reader_t *reader, u32 *out) {
    if (!reader || !out) {
        return false;
    }

    _skip_ws_and_comments(reader);
    if (reader->pos >= reader->size) {
        return false;
    }

    u8 ch = reader->data[reader->pos];
    if (!isdigit((unsigned char)ch)) {
        return false;
    }

    u64 value = 0;
    while (reader->pos < reader->size) {
        ch = reader->data[reader->pos];
        if (!isdigit((unsigned char)ch)) {
            break;
        }

        value = value * 10ULL + (u64)(ch - '0');
        if (value > UINT_MAX) {
            return false;
        }

        reader->pos++;
    }

    *out = (u32)value;
    return true;
}

bool ppm_parse_p6_blob(const void *data, size_t size, ppm_p6_blob_t *out) {
    if (!data || !out || !size) {
        return false;
    }

    memset(out, 0, sizeof(*out));

    const u8 *bytes = data;
    ppm_reader_t reader = {
        .data = bytes,
        .size = size,
        .pos = 0,
    };

    _skip_ws_and_comments(&reader);
    if (reader.pos + 2 > reader.size) {
        return false;
    }

    if (reader.data[reader.pos] != 'P' || reader.data[reader.pos + 1] != '6') {
        return false;
    }

    reader.pos += 2;
    if (reader.pos >= reader.size || !isspace((unsigned char)reader.data[reader.pos])) {
        return false;
    }

    u32 width = 0;
    u32 height = 0;
    u32 maxval = 0;

    if (
        !_parse_u32(&reader, &width) ||
        !_parse_u32(&reader, &height) ||
        !_parse_u32(&reader, &maxval)
    ) {
        return false;
    }

    if (!width || !height || maxval != 255) {
        return false;
    }

    if (reader.pos >= reader.size || !isspace((unsigned char)reader.data[reader.pos])) {
        return false;
    }

    _skip_ws_and_comments(&reader);
    if (reader.pos >= reader.size) {
        return false;
    }

    size_t pixels = (size_t)width * (size_t)height;
    if (height && pixels / height != width) {
        return false;
    }

    if (pixels > SIZE_MAX / 3) {
        return false;
    }

    size_t raster_size = pixels * 3;
    if (reader.size - reader.pos < raster_size) {
        return false;
    }

    out->width = width;
    out->height = height;
    out->raster = reader.data + reader.pos;
    out->raster_size = raster_size;

    return true;
}
