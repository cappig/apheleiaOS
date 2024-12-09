#pragma once

#include <base/types.h>
#include <gfx/psf.h>
#include <term/term.h>

typedef enum {
    // Output to an array where each element is a 16 color character
    TERM_VGA = 0,

    // Output to a framebuffer like array where each
    // element represents a single rgba pixel
    TERM_RASTER = 1,
} gfx_term_mode;

// Depending on the chosen mode the color is either
// a raw rgba value or a 16 bit vga char + attrib
typedef void (*term_draw_fn)(usize x, usize y, u32 color);

typedef struct {
    terminal* term;

    gfx_term_mode mode;

    usize pixel_width;
    usize pixel_height;

    bool draw;
    term_draw_fn draw_fn;

    // Our terminal can use a custom psf font in framebuffer mode
    // If no font is provided a hardcoded bitmap font will be used instead
    psf_font* font;
} gfx_terminal;


gfx_terminal* gfx_term_init(usize width, usize height, bool mode, psf_font* font, term_draw_fn draw);
