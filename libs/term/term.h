#pragma once

#include <base/types.h>
#include <gfx/color.h>

#define TERM_TAB_WIDTH 4

#define TERM_HISTORY_LINES 100

#define TERM_PARSER_STACK_SIZE 16

#define TERM_DEFAULT_BG (ANSI_BLACK)
#define TERM_DEFAULT_FG (ANSI_BRIGHT_GREY)

#define TERM_FLAG_BOLD   (1 << 0)
#define TERM_FLAG_FAINT  (1 << 1)
#define TERM_FLAG_ITALIC (1 << 2)

#define DEFAULT_STYLE                          \
    ((term_style){                             \
        .bg = term->palette[term->default_bg], \
        .fg = term->palette[term->default_fg], \
    })

enum term_ctrl_codes {
    TERM_CTRL_BEL = 0x07, // Ring the bell
    TERM_CTRL_BS = 0x08, // Move cursor one space left
    TERM_CTRL_HT = 0x09, // Move cursor to the next tab
    TERM_CTRL_LF = 0x0a, // Move to next line
    TERM_CTRL_FF = 0x0c, // Move cursor to start of next page
    TERM_CTRL_CR = 0x0d, // Move cursor to column 0
    TERM_CTRL_ESC = 0x1b, // Begin escape sequence
};

typedef struct {
    rgba_color bg;
    rgba_color fg;
    u8 flags;
} term_style;

typedef struct {
    term_style style;
    u32 ch;
} term_cell;

typedef struct {
    usize x;
    usize y;
} term_pos;


typedef struct {
    u8 state;

    u8 stack[TERM_PARSER_STACK_SIZE];
    u8 stack_index;

    term_style style;
} term_parser;

typedef struct terminal terminal;
typedef void (*term_putc_fn)(terminal* term, term_cell* cell, usize index);

typedef struct terminal {
    usize width;
    usize height;

    term_pos cursor;

    rgba_color palette[16];
    u8 default_bg;
    u8 default_fg;

    usize lines;
    term_cell* buffer;

    term_parser parser;

    term_putc_fn putc_fn;

    // For use in the callback
    void* private;
} terminal;


void term_redraw(terminal* term);

// NOTE: if calling these function externally term_draw() must be called to see the change
void term_clear(terminal* term, term_pos from, term_pos to);
void term_clear_screen(terminal* term);
void term_clear_line(terminal* term, usize y);
void term_scroll(terminal* term);

bool term_parse_char(terminal* term, char ch);
int term_parse(terminal* term, const char* string, usize max_size);

terminal* term_init(usize width, usize height, term_putc_fn putc_fn, void* private);
int term_resize(terminal* term, usize new_width, usize new_height);
