#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *name;
    char *data;
    size_t len;
    size_t *starts;
    size_t count;
} less_doc_t;

int less_write_all(int fd, const char *buf, size_t len);
bool less_read_all_fd(int fd, char **out_data, size_t *out_len);
bool less_build_line_index(
    const char *data,
    size_t len,
    size_t **out_starts,
    size_t *out_count
);
int less_render_page(
    const less_doc_t *doc,
    size_t rows,
    size_t cols,
    size_t *top_row
);
