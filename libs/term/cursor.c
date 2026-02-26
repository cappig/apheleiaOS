#include "cursor.h"

static size_t term_cursor_clamp_axis(size_t value, size_t limit) {
    if (!limit) {
        return 0;
    }

    if (value >= limit) {
        return limit - 1;
    }

    return value;
}

void term_cursor_set_col(size_t *cursor_x, size_t cols, int col) {
    if (!cursor_x || !cols) {
        return;
    }

    if (col < 1) {
        col = 1;
    }

    if ((size_t)col > cols) {
        col = (int)cols;
    }

    *cursor_x = (size_t)(col - 1);
}

void term_cursor_set_pos(
    size_t *cursor_x,
    size_t *cursor_y,
    size_t cols,
    size_t rows,
    int row,
    int col
) {
    if (!cursor_x || !cursor_y || !cols || !rows) {
        return;
    }

    if (row < 1) {
        row = 1;
    }

    if (col < 1) {
        col = 1;
    }

    if ((size_t)row > rows) {
        row = (int)rows;
    }

    if ((size_t)col > cols) {
        col = (int)cols;
    }

    *cursor_y = (size_t)(row - 1);
    *cursor_x = (size_t)(col - 1);
}

void term_cursor_move(
    size_t *cursor_x,
    size_t *cursor_y,
    size_t cols,
    size_t rows,
    int row_delta,
    int col_delta
) {
    if (!cursor_x || !cursor_y || !cols || !rows) {
        return;
    }

    int row = (int)*cursor_y + row_delta;
    int col = (int)*cursor_x + col_delta;

    if (row < 0) {
        row = 0;
    }

    if (col < 0) {
        col = 0;
    }

    if ((size_t)row >= rows) {
        row = (int)rows - 1;
    }

    if ((size_t)col >= cols) {
        col = (int)cols - 1;
    }

    *cursor_y = (size_t)row;
    *cursor_x = (size_t)col;
}

void term_cursor_save(
    const size_t *cursor_x,
    const size_t *cursor_y,
    size_t *saved_x,
    size_t *saved_y,
    bool *saved_valid
) {
    if (!cursor_x || !cursor_y || !saved_x || !saved_y) {
        return;
    }

    *saved_x = *cursor_x;
    *saved_y = *cursor_y;

    if (saved_valid) {
        *saved_valid = true;
    }
}

bool term_cursor_restore(
    size_t *cursor_x,
    size_t *cursor_y,
    size_t cols,
    size_t rows,
    const size_t *saved_x,
    const size_t *saved_y,
    const bool *saved_valid
) {
    if (
        !cursor_x ||
        !cursor_y ||
        !cols ||
        !rows ||
        !saved_x ||
        !saved_y ||
        (saved_valid && !*saved_valid)
    ) {
        return false;
    }

    *cursor_x = term_cursor_clamp_axis(*saved_x, cols);
    *cursor_y = term_cursor_clamp_axis(*saved_y, rows);
    return true;
}

void term_cursor_clamp(
    size_t *cursor_x,
    size_t *cursor_y,
    size_t *saved_x,
    size_t *saved_y,
    bool *saved_valid,
    size_t cols,
    size_t rows
) {
    if (!cursor_x || !cursor_y || !saved_x || !saved_y) {
        return;
    }

    if (!cols || !rows) {
        *cursor_x = 0;
        *cursor_y = 0;
        *saved_x = 0;
        *saved_y = 0;

        if (saved_valid) {
            *saved_valid = false;
        }

        return;
    }

    *cursor_x = term_cursor_clamp_axis(*cursor_x, cols);
    *cursor_y = term_cursor_clamp_axis(*cursor_y, rows);

    if (!saved_valid || *saved_valid) {
        *saved_x = term_cursor_clamp_axis(*saved_x, cols);
        *saved_y = term_cursor_clamp_axis(*saved_y, rows);
    }
}
