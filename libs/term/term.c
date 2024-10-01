#include "term.h"

#include <alloc/global.h>
#include <base/types.h>
#include <ctype.h>
#include <gfx/color.h>
#include <gfx/vga.h>
#include <stdlib.h>
#include <string.h>

#include "palette.h"
#include "parser.h"


static void _term_buffer_place(terminal* term, const char ch) {
    usize index = term->cursor.x + term->cursor.y * term->width;
    term->buffer[index] = (term_char){
        .style = term->parser.style,
        .ascii = ch,
    };
}

void term_putc(terminal* term, const char ch) {
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


void term_draw(terminal* term) {
    for (usize i = 0; i < term->width * term->height; i++) {
        term_char* ch = &term->buffer[i];

        if (isprint(ch->ascii))
            term->putc_fn(*ch, i);
    }
}


void term_clear(terminal* term, term_pos from, term_pos to) {
    usize from_index = from.x + term->width * from.y;
    usize to_index = to.x + term->width * to.y;

    for (usize i = from_index; i <= to_index; i++) {
        term_char* ch = &term->buffer[i];

        ch->style = DEFAULT_STYLE;
        ch->ascii = ' ';
    }
}

void term_clear_screen(terminal* term) {
    term_pos from = {0, 0};
    term_pos to = {term->width - 1, term->height - 1};

    term_clear(term, from, to);
}

void term_clear_line(terminal* term, usize y) {
    term_pos from = {0, y};
    term_pos to = {term->width - 1, y};

    term_clear(term, from, to);
}

void term_scroll(terminal* term) {
    // Move one line up
    usize scroll_size = (term->width * term->height) - term->width;
    for (usize i = 0; i < scroll_size; i++)
        term->buffer[i] = term->buffer[i + term->width];

    // Clear the bottom line
    term_clear_line(term, term->height - 1);

    if (term->cursor.y > 0)
        term->cursor.y--;
}


terminal* term_init(usize width, usize height, term_putc_fn putc_fn) {
    terminal* term = gcalloc(sizeof(terminal));
    if (!term)
        return NULL;

    term->putc_fn = putc_fn;

    term->default_bg = TERM_DEFAULT_BG;
    term->default_fg = TERM_DEFAULT_FG;

    term->parser.style = DEFAULT_STYLE;

    term->width = width;
    term->height = height;

    term->lines = height + TERM_HISTORY_LINES;

    term->buffer = gmalloc(term->lines * width * sizeof(term_char));
    if (!term->buffer)
        return NULL;

    for (usize y = 0; y < term->lines; y++)
        term_clear_line(term, y);

    term_set_palette(term, default_ansi_colors);

    return term;
}

// FIXME: handle resizing to smaller size
int term_resize(terminal* term, usize new_width, usize new_height) {
    usize old_height = term->height;
    usize old_width = term->width;

    if (new_height == old_height && new_width == old_width)
        return 0;

    term_char* old_buffer = term->buffer;

    term->width = new_width;
    term->height = new_height;

    usize old_lines = term->lines;

    term->lines = new_height + TERM_HISTORY_LINES;
    term->buffer = gmalloc(term->lines * new_width * sizeof(term_char));

    for (usize y = 0; y < term->lines; y++) {
        term_clear_line(term, y);

        if (y >= old_lines)
            continue;

        void* dest = &term->buffer[y * new_width];
        void* src = &old_buffer[y * old_width];

        memcpy(dest, src, old_width * sizeof(term_char));
    }

    gfree(old_buffer);

    term_draw(term);

    return 1;
}


int term_parse(terminal* term, const char* string, usize max_size) {
    usize printed = 0;

    for (usize i = 0; string[i] && i < max_size; i++) {
        char ch = parse_ansi_char(term, &string[i]);

        if (ch) {
            term_putc(term, ch);
            printed++;
        }
    }

    term_draw(term);

    return printed;
}
