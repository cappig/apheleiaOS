#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    size_t *x;
    size_t *y;
    size_t cols;
    size_t rows;
} term_cursor_t;

typedef struct {
    size_t *x;
    size_t *y;
    bool *valid;
} term_saved_cursor_t;

void term_cursor_set_col(size_t *cursor_x, size_t cols, int col);

void term_cursor_set_pos(size_t *cursor_x, size_t *cursor_y, size_t cols, size_t rows, int row, int col);

void term_cursor_move(size_t *cursor_x, size_t *cursor_y, size_t cols, size_t rows, int row_delta, int col_delta);

void term_cursor_save(
    const size_t *cursor_x,
    const size_t *cursor_y,
    size_t *saved_x,
    size_t *saved_y,
    bool *saved_valid
);

bool term_cursor_restore(term_cursor_t *cursor, const term_saved_cursor_t *saved);

void term_cursor_clamp(term_cursor_t *cursor, term_saved_cursor_t *saved);
