#include "render.h"

#include <alloc/global.h>
#include <base/macros.h>
#include <base/types.h>
#include <gfx/color.h>
#include <gfx/font.h>
#include <gfx/psf.h>
#include <stdlib.h>

#include "palette.h"
#include "term.h"


static void _putc_vga(terminal* term, term_cell* cell, usize index) {
    gfx_terminal* gfx = term->private;
    if (!gfx)
        return;

    if (!gfx->draw || !gfx->draw_fn)
        return;

    usize char_x = index % gfx->pixel_width;
    usize char_y = index / gfx->pixel_width;

    u16 attr = term_cell_to_vga(cell);
    u16 vga = (u8)cell->ch | (attr << 8);

    gfx->draw_fn(char_x, char_y, vga);
}


static void _draw_vesa_psf(gfx_terminal* term, u32 x, u32 y, term_cell* cell) {
    if (!term->draw || !term->draw_fn)
        return;

    psf_font* font = term->font;

    const u8* glyph = (u8*)font->data + cell->ch * font->glyph_size;

    usize line_width = DIV_ROUND_UP(font->glyph_width, 8);

    for (usize h = 0; h < font->glyph_height; h++) {
        usize mask = 1 << font->glyph_width;

        for (usize w = 0; w < font->glyph_width; w++) {
            rgba_color color;

            if (*glyph & mask)
                color = cell->style.fg;
            else
                color = cell->style.bg;

            usize pix_x = x + w;
            usize pix_y = y + h;

            term->draw_fn(pix_x, pix_y, color.raw);

            mask >>= 1;
        }

        glyph += line_width;
    }
}

static void _putc_vesa_psf(terminal* term, term_cell* cell, usize index) {
    gfx_terminal* gfx = term->private;
    if (!gfx)
        return;

    psf_font* font = gfx->font;

    usize x = index % term->width;
    usize y = index / term->width;

    _draw_vesa_psf(gfx, x * font->glyph_width, y * font->glyph_height, cell);
}


static void _draw_vesa_header(gfx_terminal* term, u32 x, u32 y, term_cell* cell) {
    if (!term->draw || !term->draw_fn)
        return;

    const u8* glyph = header_font_bitmap[cell->ch - 32];

    for (usize h = 0; h < HEADER_FONT_HEIGHT; h++) {
        for (usize w = HEADER_FONT_WIDTH; w > 0; w--) {
            rgba_color color;

            if (glyph[h] & (1 << w))
                color = cell->style.fg;
            else
                color = cell->style.bg;

            usize pix_x = x + (HEADER_FONT_WIDTH - w);
            usize pix_y = y + h;

            term->draw_fn(pix_x, pix_y, color.raw);
        }
    }
}

static void _putc_vesa_header(terminal* term, term_cell* cell, usize index) {
    gfx_terminal* gfx = term->private;

    usize x = index % term->width;
    usize y = index / term->width;

    _draw_vesa_header(gfx, x * HEADER_FONT_WIDTH, y * HEADER_FONT_HEIGHT, cell);
}

gfx_terminal*
gfx_term_init(usize width, usize height, bool mode, psf_font* font, term_draw_fn draw) {
    if (!width || !height || !draw)
        return NULL;

    gfx_terminal* gfx = gcalloc(sizeof(gfx_terminal));
    if (!gfx)
        return NULL;

    gfx->draw_fn = draw;

    gfx->pixel_height = height;
    gfx->pixel_width = width;

    term_putc_fn putc_fn;

    // The proveided size values are in pixels if the framebuffer mode is used
    // They have to be scaled by the size of the font
    if (mode == TERM_RASTER) {
        if (font) {
            gfx->font = font;

            width /= font->glyph_width;
            height /= font->glyph_height;

            putc_fn = _putc_vesa_psf;
        } else {
            width /= HEADER_FONT_WIDTH;
            height /= HEADER_FONT_HEIGHT;

            putc_fn = _putc_vesa_header;
        }
    } else {
        putc_fn = _putc_vga;
    }

    gfx->term = term_init(width, height, putc_fn, gfx);
    if (!gfx->term)
        return NULL;

    return gfx;
}
