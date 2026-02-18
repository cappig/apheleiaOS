#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef bool (*textdb_line_match_fn)(const char *line, void *ctx);

const char *textdb_next_field(const char *cursor, char *out, size_t out_len);
int textdb_find_line(
    const char *text,
    char *line_buf,
    size_t line_buf_len,
    textdb_line_match_fn match,
    void *ctx
);
