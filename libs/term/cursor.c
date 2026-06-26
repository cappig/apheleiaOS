#include "cursor.h"

static size_t clamp_axis(size_t value, size_t limit) {
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

void term_cursor_set_pos(size_t *cursor_x, size_t *cursor_y, size_t cols, size_t rows, int row, int col) {
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

void term_cursor_move(size_t *cursor_x, size_t *cursor_y, size_t cols, size_t rows, int row_delta, int col_delta) {
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

bool term_cursor_restore(term_cursor_t *cursor, const term_saved_cursor_t *saved) {
    bool have_saved_pos = saved && saved->x && saved->y;
    bool saved_ok = !saved || !saved->valid || *saved->valid;

    if (!cursor || !cursor->x || !cursor->y || !cursor->cols || !cursor->rows || !have_saved_pos || !saved_ok) {
        return false;
    }

    *cursor->x = clamp_axis(*saved->x, cursor->cols);
    *cursor->y = clamp_axis(*saved->y, cursor->rows);
    return true;
}

void term_cursor_clamp(term_cursor_t *cursor, term_saved_cursor_t *saved) {
    if (!cursor || !cursor->x || !cursor->y || !saved || !saved->x || !saved->y) {
        return;
    }

    if (!cursor->cols || !cursor->rows) {
        *cursor->x = 0;
        *cursor->y = 0;
        *saved->x = 0;
        *saved->y = 0;

        if (saved->valid) {
            *saved->valid = false;
        }

        return;
    }

    *cursor->x = clamp_axis(*cursor->x, cursor->cols);
    *cursor->y = clamp_axis(*cursor->y, cursor->rows);

    if (!saved->valid || *saved->valid) {
        *saved->x = clamp_axis(*saved->x, cursor->cols);
        *saved->y = clamp_axis(*saved->y, cursor->rows);
    }
}
