#pragma once

#include <base/types.h>
#include <stddef.h>

typedef struct {
    u32 codepoint;
    u8 fg;
    u8 bg;
} term_cell_t;

void term_cell_set_blank(term_cell_t *cell, u8 fg, u8 bg);

void term_cells_clear_range(
    term_cell_t *cells,
    size_t count,
    size_t start,
    size_t end,
    u8 fg,
    u8 bg
);

void term_cells_clear(term_cell_t *cells, size_t cols, size_t rows, u8 fg, u8 bg);

void term_cells_scroll_up(
    term_cell_t *cells,
    size_t cols,
    size_t rows,
    u8 fg,
    u8 bg
);
