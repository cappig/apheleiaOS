#include "cells.h"

#include <string.h>

void term_cell_set_blank(term_cell_t *cell, u8 fg, u8 bg) {
    if (!cell) {
        return;
    }

    cell->codepoint = ' ';
    cell->fg = fg;
    cell->bg = bg;
}

void term_cells_clear_range(
    term_cell_t *cells,
    size_t count,
    size_t start,
    size_t end,
    u8 fg,
    u8 bg
) {
    if (!cells || !count || start >= end || start >= count) {
        return;
    }

    if (end > count) {
        end = count;
    }

    for (size_t i = start; i < end; i++) {
        term_cell_set_blank(&cells[i], fg, bg);
    }
}

void term_cells_clear(term_cell_t *cells, size_t cols, size_t rows, u8 fg, u8 bg) {
    if (!cells || !cols || !rows) {
        return;
    }

    size_t count = cols * rows;
    if (count / cols != rows) {
        return;
    }

    term_cells_clear_range(cells, count, 0, count, fg, bg);
}

void term_cells_scroll_up(
    term_cell_t *cells,
    size_t cols,
    size_t rows,
    u8 fg,
    u8 bg
) {
    if (!cells || !cols || !rows) {
        return;
    }

    size_t count = cols * rows;
    if (count / cols != rows) {
        return;
    }

    if (rows > 1) {
        memmove(cells, cells + cols, (rows - 1) * cols * sizeof(*cells));
    }

    term_cells_clear_range(cells, count, (rows - 1) * cols, count, fg, bg);
}
