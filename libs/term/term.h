#pragma once

#include <base/types.h>
#include <gfx/color.h>

#define TERM_TAB_WIDTH 4

#define TERM_HISTORY_LINES 100

#define TERM_DEFAULT_BG (ANSI_BLACK)
#define TERM_DEFAULT_FG (ANSI_BRIGHT_GREY)

#define TERM_FLAG_BOLD   (1 << 0)
#define TERM_FLAG_FAINT  (1 << 1)
#define TERM_FLAG_ITALIC (1 << 2)


typedef struct {
    u8 flags;
    rgba_color bg;
    rgba_color fg;
} term_style;

typedef struct {
    term_style style;
    u8 ascii;
} term_char;

typedef term_char* term_line;


typedef struct {
    u8 state;

    // The accumulator. Collects the numbers in the sequence.
    // acc_flags tells us what the acc is currently storing
    u8 acc_content;
    u8 acc;

    term_style current;
    term_style next;
} ansi_parser;


typedef void (*term_putc_fn)(term_char ch, usize index);

typedef struct {
    usize width;
    usize height;

    usize cur_x;
    usize cur_y;

    rgba_color palette[16];
    usize default_bg;
    usize default_fg;

    usize lines;
    term_char* buffer;

    ansi_parser parser;

    term_putc_fn term_putc;
} terminal;


void term_scroll(terminal* term);
void term_clear(terminal* term);

terminal* term_init(usize width, usize height, term_putc_fn putc_fn);
int term_resize(terminal* term, usize new_width, usize new_height);

int term_parse(terminal* term, const char* string, usize max_size);
