#include "term.h"

#include <alloc/global.h>
#include <base/types.h>
#include <gfx/color.h>
#include <gfx/vga.h>
#include <stdlib.h>
#include <string.h>

#include "palette.h"
#include "parser.h"


void term_redraw(terminal* term) {
    if (!term->putc_fn)
        return;

    for (usize i = 0; i < term->width * term->height; i++) {
        term_cell* cell = &term->buffer[i];

        term->putc_fn(term, cell, i);
    }
}

static void _term_buffer_place(terminal* term, const u32 ch) {
    usize index = term->cursor.x + term->cursor.y * term->width;

    term_cell cell = {term->parser.style, ch};

    term->buffer[index] = cell;

    if (term->putc_fn)
        term->putc_fn(term, &cell, index);
}

static void _term_putc(terminal* term, const u32 ch) {
    switch (ch) {
    case '\r':
        term->cursor.x = 0;
        break;

    case '\n':
        term->cursor.x = 0;
        term->cursor.y++;
        break;

    case '\t':
        term->cursor.x += TERM_TAB_WIDTH;
        break;

    case '\b':
        term->cursor.x--;
        break;

    case '\v':
        term->cursor.y++;
        break;

    default:
        _term_buffer_place(term, ch);
        term->cursor.x++;
        break;
    }

    // Wrap line, TODO: add ability to disable wrapping
    if (term->cursor.x >= term->width) {
        term->cursor.y++;
        term->cursor.x = 0;
    }

    // Scroll one line up
    if (term->cursor.y >= term->height)
        term_scroll(term);
}


void term_clear(terminal* term, term_pos from, term_pos to) {
    usize from_index = from.x + term->width * from.y;
    usize to_index = to.x + term->width * to.y;

    for (usize i = from_index; i < to_index; i++) {
        term_cell* cell = &term->buffer[i];

        cell->style = DEFAULT_STYLE;
        cell->ch = ' ';

        if (term->putc_fn)
            term->putc_fn(term, cell, i);
    }
}

void term_clear_screen(terminal* term) {
    term_pos from = {0, 0};
    term_pos to = {term->width, term->height};

    term_clear(term, from, to);
}

void term_clear_line(terminal* term, usize y) {
    term_pos from = {0, y};
    term_pos to = {term->width, y};

    term_clear(term, from, to);
}

void term_scroll(terminal* term) {
    // Move one line up
    usize scroll_size = (term->width * term->height) - term->width;

    for (usize i = 0; i < scroll_size; i++) {
        term->buffer[i] = term->buffer[i + term->width];

        if (term->putc_fn)
            term->putc_fn(term, &term->buffer[i], i);
    }

    // Clear the bottom line
    term_clear_line(term, term->height - 1);

    if (term->cursor.y > 0)
        term->cursor.y--;
}


terminal* term_init(usize width, usize height, term_putc_fn putc_fn, void* private) {
    terminal* term = gcalloc(sizeof(terminal));
    if (!term)
        return NULL;

    term->putc_fn = putc_fn;

    term->private = private;

    term->default_bg = TERM_DEFAULT_BG;
    term->default_fg = TERM_DEFAULT_FG;

    term->parser.style = DEFAULT_STYLE;

    term->width = width;
    term->height = height;

    term->lines = height;

    term->buffer = gmalloc(term->lines * width * sizeof(term_cell));
    if (!term->buffer)
        return NULL;

    term_set_palette(term, default_ansi_colors);

    for (usize y = 0; y < term->lines; y++)
        term_clear_line(term, y);

    return term;
}

// FIXME: handle resizing to smaller size
int term_resize(terminal* term, usize new_width, usize new_height) {
    usize old_height = term->height;
    usize old_width = term->width;

    if (new_height == old_height && new_width == old_width)
        return 0;

    term_cell* old_buffer = term->buffer;

    term->width = new_width;
    term->height = new_height;

    usize old_lines = term->lines;

    term->lines = new_height + TERM_HISTORY_LINES;
    term->buffer = gmalloc(term->lines * new_width * sizeof(term_cell));

    for (usize y = 0; y < term->lines; y++) {
        term_clear_line(term, y);

        if (y >= old_lines)
            continue;

        void* dest = &term->buffer[y * new_width];
        void* src = &old_buffer[y * old_width];

        memcpy(dest, src, old_width * sizeof(term_cell));
    }

    gfree(old_buffer);

    // term_redraw(term);

    return 1;
}


bool term_parse_char(terminal* term, char ch) {
    char print = parse_ansi_char(term, ch);

    if (print) {
        _term_putc(term, print);
        return true;
    }

    return false;
}

int term_parse(terminal* term, const char* string, usize max_size) {
    usize printed = 0;

    for (usize i = 0; string[i] && i < max_size; i++)
        printed += term_parse_char(term, string[i]);

    return printed;
}
