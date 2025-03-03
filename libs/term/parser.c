#include "parser.h"

#include <alloc/global.h>
#include <base/types.h>
#include <ctype.h>
#include <gfx/color.h>
#include <gfx/vga.h>
#include <stdlib.h>
#include <string.h>

#include "term.h"


static void _handle_color(terminal* term, usize num) {
    term_style* style = &term->parser.style;

    switch (num) {
    case 0:
        *style = DEFAULT_STYLE;
        break;

    case 7:
        memswap(&style->fg, &style->bg, sizeof(rgba_color));
        break;

    case 30 ... 37:
        style->fg = term->palette[num - 30];
        break;
    case 39:
        style->fg = term->palette[term->default_fg];
        break;

    case 40 ... 47:
        style->bg = term->palette[num - 40];
        break;
    case 49:
        style->bg = term->palette[term->default_bg];
        break;

    case 90 ... 97:
        style->fg = term->palette[num - 90 + 8];
        break;

    case 100 ... 107:
        style->bg = term->palette[num - 100 + 8];
        break;
    }
}

static void _handle_flags(terminal* term, usize num) {
    term_style* style = &term->parser.style;

    switch (num) {
    // Set flags
    case 1:
        style->flags |= TERM_CHAR_BOLD;
        break;
    case 2:
        style->flags |= TERM_CHAR_FAINT;
        break;
    case 3:
        style->flags |= TERM_CHAR_ITALIC;
        break;

    // Unset flags
    case 21:
        style->flags &= ~(TERM_CHAR_BOLD);
        break;
    case 22:
        style->flags &= ~(TERM_CHAR_FAINT);
        break;
    case 23:
        style->flags &= ~(TERM_CHAR_ITALIC);
        break;
    }
}

static void _handle_sgr_number(terminal* term, usize num) {
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

// Increment the index if there are more things left in the stack. If not fail
#define INCREMENT_OR_FAIL(index, max_index) \
    ({                                      \
        if (++(index) >= (max_index))       \
            return false;                   \
    })

static bool _handle_sgr(terminal* term) {
    term_parser* parser = &term->parser;

    for (usize i = 0; i < parser->stack_index; i++) {
        usize num = parser->stack[i];

        // Special cases that read more args from the stack
        // This is a bit fucky because we have to check the index each time we pop the stack
        if (num == 38 || num == 48) {
            INCREMENT_OR_FAIL(i, parser->stack_index);
            u8 selector = parser->stack[i];

            rgba_color color = {0};

            // Use the ANSI LUT colors
            if (selector == 5) {
                INCREMENT_OR_FAIL(i, parser->stack_index);
                u8 index = parser->stack[i];

                color = ansi_to_rgb(index);
            }
            // Use RGB color
            else if (selector == 2) {
                INCREMENT_OR_FAIL(i, parser->stack_index);
                u8 red = parser->stack[i];
                INCREMENT_OR_FAIL(i, parser->stack_index);
                u8 green = parser->stack[i];
                INCREMENT_OR_FAIL(i, parser->stack_index);
                u8 blue = parser->stack[i];

                color = rgb_to_color(red, green, blue);
            } else {
                continue;
            }

            if (num == 38)
                term->parser.style.fg = color;
            else
                term->parser.style.bg = color;
        } else {
            _handle_sgr_number(term, parser->stack[i]);
        }
    }

    return true;
}

static void _handle_cur_mov(terminal* term) {
    term_parser* parser = &term->parser;

    usize y = clamp(parser->stack[0], 1, term->height);
    usize x = clamp(parser->stack[1], 1, term->width);

    term->cursor.y = y - 1;
    term->cursor.x = x - 1;
}

static void _csi_erase_line(terminal* term) {
    switch (term->parser.stack[0]) {
    default:
    case 0: // from cursor to end of line
        term_pos to = {term->width - 1, term->cursor.y};
        term_clear(term, term->cursor, to);
        break;

    case 1: // from cursor to beginning of the line
        term_pos from = {0, term->cursor.y};
        term_clear(term, from, term->cursor);
        break;

    case 2: // entire line
        term_clear_line(term, term->cursor.y);
        break;
    }
}

static void _csi_erase_screen(terminal* term) {
    switch (term->parser.stack[0]) {
    default:
    case 0: // from cursor to end of screen
        term_pos to = {term->width - 1, term->height - 1};
        term_clear(term, term->cursor, to);
        break;

    case 1: // from cursor to beginning of the screen
        term_pos from = {0, 0};
        term_clear(term, from, term->cursor);
        break;

    case 2: // entire screen
        term_clear_screen(term);
        break;
    }
}

static bool _handle_csi(terminal* term, char ch) {
    // TODO: add the missing commands
    switch (ch) {
    case 'm': // Select graphic rendition
        return _handle_sgr(term);

    case 'H': // Move cursor
    case 'f':
        _handle_cur_mov(term);
        return true;

    case 'K': // Erase line
        _csi_erase_line(term);
        return true;

    case 'J': // Erase screen
        _csi_erase_screen(term);
        return true;

    default:
        return false;
    }
}


#define STATE_ESC 0
#define STATE_CSI 1
#define STATE_END 2

char parse_ansi_char(terminal* term, const char ch) {
    term_parser* parser = &term->parser;

    char ret = '\0';

    switch (parser->state) {
    case STATE_ESC:
        if (ch == TERM_CTRL_ESC)
            parser->state = STATE_CSI;
        else
            ret = ch;
        break;

    case STATE_CSI:
        if (ch == '[') {
            parser->state = STATE_END;
        } else {
            parser->state = STATE_ESC;
            ret = ch;
        }
        break;

    case STATE_END:
        if (isdigit(ch)) {
            parser->stack[parser->stack_index] *= 10;
            parser->stack[parser->stack_index] += ch - '0';
        } else if (ch == ';') {
            if (parser->stack_index < TERM_PARSER_STACK_SIZE)
                parser->stack_index++;
            else
                parser->state = STATE_ESC;
        } else {
            _handle_csi(term, ch);

            memset(parser->stack, 0, TERM_PARSER_STACK_SIZE);
            parser->stack_index = 0;

            parser->state = STATE_ESC;
        }

        break;
    }

    return ret;
}
