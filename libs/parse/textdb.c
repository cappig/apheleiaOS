#include <parse/textdb.h>
#include <string.h>

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

int textdb_find_line(
    const char *text,
    char *line_buf,
    size_t line_buf_len,
    textdb_line_match_fn match,
    void *ctx
) {
    if (!text || !line_buf || line_buf_len < 2 || !match) {
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

        if (!line_len || line[0] == '#') {
            continue;
        }

        if (line_len >= line_buf_len) {
            line_len = line_buf_len - 1;
        }

        memcpy(line_buf, line, line_len);
        line_buf[line_len] = '\0';

        if (match(line_buf, ctx)) {
            return 0;
        }
    }

    return -1;
}
