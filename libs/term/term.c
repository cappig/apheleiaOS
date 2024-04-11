#include "term.h"

#include <alloc/global.h>
#include <base/types.h>
#include <ctype.h>
#include <gfx/color.h>
#include <gfx/vga.h>
#include <log/log.h>
#include <string.h>

#include "palette.h"

#define DEFAULT_STYLE                          \
    ((term_style){                             \
        .bg = term->palette[term->default_bg], \
        .fg = term->palette[term->default_fg], \
    })


static void _term_buffer_place(terminal* term, const char ch) {
    usize index = term->cur_x + term->cur_y * term->width;

    term->buffer[index] = (term_char){
        .style = term->parser.current,
        .ascii = ch,
    };
}

static void _term_putc(terminal* term, const char ch) {
    switch (ch) {
    case '\r':
        term->cur_x = 0;
        break;
    case '\n':
        term->cur_x = 0;
        term->cur_y++;
        break;
    case '\t':
        term->cur_x += TERM_TAB_WIDTH;
        break;
    case '\v':
        term->cur_y++;
        break;
    default:
        _term_buffer_place(term, ch);
        term->cur_x++;
        break;
    }

    // Wrap line
    if (term->cur_x >= term->width) {
        term->cur_y++;
        term->cur_x = 0;
    }

    // Scroll one line up
    if (term->cur_y >= term->height)
        term_scroll(term);
}

static void _handle_color(terminal* term, usize num) {
    term_style* next = &term->parser.next;

    switch (num) {
    case 0:
        *next = DEFAULT_STYLE;
        break;

    case 7:
        memswap(&next->fg, &next->bg, sizeof(rgba_color));
        break;

    case 30 ... 37:
        next->fg = term->palette[num - 30];
        break;
    case 38:
        // TODO:
        break;
    case 39:
        next->fg = term->palette[term->default_fg];
        break;

    case 40 ... 47:
        next->bg = term->palette[num - 40];
        break;
    case 48:
        // TODO:
        break;
    case 49:
        next->bg = term->palette[term->default_bg];
        break;

    case 90 ... 97:
        next->fg = term->palette[num - 90 + 8];
        break;

    case 100 ... 107:
        next->bg = term->palette[num - 100 + 8];
        break;
    }
}

static void _handle_flags(terminal* term, usize num) {
    term_style* next = &term->parser.next;

    switch (num) {
    // Set flags
    case 1:
        next->flags |= TERM_FLAG_BOLD;
        break;
    case 2:
        next->flags |= TERM_FLAG_FAINT;
        break;
    case 3:
        next->flags |= TERM_FLAG_ITALIC;
        break;

    // unset flags
    case 21:
        next->flags &= ~(TERM_FLAG_BOLD);
        break;
    case 22:
        next->flags &= ~(TERM_FLAG_FAINT);
        break;
    case 23:
        next->flags &= ~(TERM_FLAG_ITALIC);
        break;
    }
}

static void _handle_csi(terminal* term, usize num) {
    switch (num) {
    case 1 ... 3: // set flags
    case 21 ... 23: // unset flags
        _handle_flags(term, num);
        break;

    case 0: // reset
    case 7: // invert
    case 30 ... 39: // fg
    case 40 ... 49: // bg
    case 90 ... 97: // fg bright
    case 100 ... 107: // bg bright
        _handle_color(term, num);
        break;
    }
}

#define STATE_ESC     0
#define STATE_BRACKET 1
#define STATE_CSI     2
#define STATE_END     3

#define ACC_NOTHING 0
#define ACC_CSI     1

static char _parse_char(terminal* term, const char ch) {
    char ret = '\0';

    switch (term->parser.state) {
    case STATE_ESC:
        if (ch == '\x1b')
            term->parser.state = STATE_BRACKET;
        else
            ret = ch;
        break;

    case STATE_BRACKET:
        if (ch == '[') {
            term->parser.acc_content = ACC_CSI;
            term->parser.acc = 0;
            term->parser.state = STATE_CSI;
        } else {
            term->parser.state = STATE_ESC;
            ret = ch;
        }
        break;

    case STATE_CSI:
        if (isdigit(ch)) {
            term->parser.acc *= 10;
            term->parser.acc += ch - '0';
        } else {
            term->parser.state = STATE_END;
        }
        break;


    case STATE_END:
        break;
    }

    if (term->parser.state == STATE_END) {
        if (term->parser.acc_content == ACC_CSI)
            _handle_csi(term, term->parser.acc);

        term->parser.acc_content = ACC_NOTHING;
        term->parser.acc = 0;

        if (ch == ';') {
            term->parser.acc_content = ACC_CSI;
            term->parser.state = STATE_CSI;
        } else if (ch == 'm') {
            term->parser.state = STATE_ESC;
            term->parser.current = term->parser.next;
        } else {
            term->parser.state = STATE_ESC;
            term->parser.next = term->parser.current;
            ret = ch;
        }
    }

    return ret;
}

static void _clear_line(terminal* term, usize y) {
    for (usize i = 0; i < term->width; i++) {
        term_char* ch = &term->buffer[y * term->width + i];

        ch->style = DEFAULT_STYLE;
        ch->ascii = ' ';
    }
}

static void _term_write(terminal* term) {
    for (usize i = 0; i < term->width * term->height; i++) {
        term_char* ch = &term->buffer[i];

        if (isprint(ch->ascii))
            term->term_putc(*ch, i);
    }
}


void term_scroll(terminal* term) {
    // Move one line up
    for (usize i = 0; i < ((term->width * term->height) - term->width); i++) {
        term->buffer[i] = term->buffer[i + term->width];
    }

    // Clear the bottom line
    _clear_line(term, (term->height - 1));

    if (term->cur_y > 0)
        term->cur_y--;
}

void term_clear(terminal* term) {
    for (usize y = 0; y < term->height; y++)
        _clear_line(term, y);
}

terminal* term_init(usize width, usize height, term_putc_fn putc_fn) {
    terminal* term = gcalloc(sizeof(terminal));

    term->term_putc = putc_fn;

    term->default_bg = TERM_DEFAULT_BG;
    term->default_fg = TERM_DEFAULT_FG;

    term->parser.current = DEFAULT_STYLE;
    term->parser.next = DEFAULT_STYLE;

    term->width = width;
    term->height = height;

    term->lines = height + TERM_HISTORY_LINES;
    term->buffer = gmalloc(term->lines * width * sizeof(term_char));

    for (usize y = 0; y < term->lines; y++)
        _clear_line(term, y);

    term_set_palette(term, default_ansi_colors);

    return term;
}

int term_resize(terminal* term, usize new_width, usize new_height) {
    // FIXME: handle resizing to smaller size
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
        _clear_line(term, y);

        if (y >= old_lines)
            continue;

        void* dest = &term->buffer[y * new_width];
        void* src = &old_buffer[y * old_width];

        memcpy(dest, src, old_width * sizeof(term_char));
    }

    gfree(old_buffer);

    _term_write(term);

    return 1;
}

int term_parse(terminal* term, const char* string, usize max_size) {
    usize printed = 0;

    for (usize i = 0; string[i] && i < max_size; i++) {
        char ch = _parse_char(term, string[i]);

        if (ch) {
            _term_putc(term, ch);
            printed++;
        }
    }

    _term_write(term);

    return printed;
}
