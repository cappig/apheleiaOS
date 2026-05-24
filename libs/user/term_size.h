#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    size_t rows;
    size_t cols;
} term_size_t;

typedef bool (*term_read_byte_fn)(
    int fd,
    char *out,
    int timeout_ms,
    void *ctx
);

typedef void (*term_push_byte_fn)(int fd, char ch, void *ctx);

bool term_size_ok(const term_size_t *size);
void term_get_size(
    int input_fd,
    int output_fd,
    term_size_t *out,
    const term_size_t *fallback
);
bool term_set_size(int fd, const term_size_t *size);
bool term_probe_size(
    int input_fd,
    int output_fd,
    term_size_t *out,
    term_read_byte_fn read_byte,
    term_push_byte_fn push_byte,
    void *ctx
);
