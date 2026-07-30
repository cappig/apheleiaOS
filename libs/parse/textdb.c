#include <errno.h>
#include <parse/textdb.h>
#include <string.h>

#define TEXTDB_LINE_MAX 256

const char *textdb_next_field(const char *cursor, char *out, size_t out_len) {
    size_t len = 0;

    if (!out_len) {
        return cursor;
    }

    while (*cursor && *cursor != ':' && *cursor != '\n') {
        if (len + 1 < out_len) {
            out[len++] = *cursor;
        }

        cursor++;
    }

    out[len] = '\0';

    if (*cursor == ':') {
        cursor++;
    }

    return cursor;
}

int textdb_copy_field(char **cursor, size_t *left, char **out, const char *src) {
    if (!cursor || !left || !out || !src) {
        return ERANGE;
    }

    size_t len = strlen(src) + 1;
    if (len > *left) {
        return ERANGE;
    }

    memcpy(*cursor, src, len);
    *out = *cursor;
    *cursor += len;
    *left -= len;

    return 0;
}

int textdb_scan(const char *text, textdb_line_match_fn match, void *ctx) {
    if (!text || !match) {
        return -1;
    }

    const char *cursor = text;

    while (*cursor) {
        const char *line = cursor;

        while (*cursor && *cursor != '\n') {
            cursor++;
        }

        size_t line_len = (size_t)(cursor - line);
        if (*cursor == '\n') {
            cursor++;
        }

        // a truncated record could match the wrong entry, so drop it whole
        if (!line_len || line[0] == '#' || line_len >= TEXTDB_LINE_MAX) {
            continue;
        }

        char line_buf[TEXTDB_LINE_MAX];
        memcpy(line_buf, line, line_len);
        line_buf[line_len] = '\0';

        if (match(line_buf, ctx)) {
            return 0;
        }
    }

    return -1;
}
