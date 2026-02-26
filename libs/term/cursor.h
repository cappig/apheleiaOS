#pragma once

#include <stdbool.h>
#include <stddef.h>

void term_cursor_set_col(size_t *cursor_x, size_t cols, int col);

void term_cursor_set_pos(
    size_t *cursor_x,
    size_t *cursor_y,
    size_t cols,
    size_t rows,
    int row,
    int col
);

void term_cursor_move(
    size_t *cursor_x,
    size_t *cursor_y,
    size_t cols,
    size_t rows,
    int row_delta,
    int col_delta
);

void term_cursor_save(
    const size_t *cursor_x,
    const size_t *cursor_y,
    size_t *saved_x,
    size_t *saved_y,
    bool *saved_valid
);

bool term_cursor_restore(
    size_t *cursor_x,
    size_t *cursor_y,
    size_t cols,
    size_t rows,
    const size_t *saved_x,
    const size_t *saved_y,
    const bool *saved_valid
);

void term_cursor_clamp(
    size_t *cursor_x,
    size_t *cursor_y,
    size_t *saved_x,
    size_t *saved_y,
    bool *saved_valid,
    size_t cols,
    size_t rows
);
