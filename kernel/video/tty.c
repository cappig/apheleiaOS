#include <base/types.h>
#include <gfx/font.h>
#include <gfx/vesa.h>
#include <gfx/vga.h>
#include <stddef.h>
#include <term/palette.h>
#include <term/term.h>
#include <x86/serial.h>

#include "gfx/state.h"

static terminal* term = NULL;
static graphics_state* gfx = NULL;


static void _putc_vga(term_char ch, usize index) {
    static volatile u16* vga_base = (u16*)VGA_ADDR;

    u16 attr = term_char_to_vga(ch);
    vga_base[index] = ch.ascii | (attr << 8);
}

static void _putc_vesa(term_char ch, usize index) {
    usize x = index % term->width;
    usize y = index / term->width;

    vesa_putc(gfx, x * FONT_WIDTH, y * FONT_HEIGHT, ch);
}


void puts(const char* s) {
    send_serial_string(s);

    if (term)
        term_parse(term, s, (usize)-1);
}

void tty_init(graphics_state* gfx_ptr) {
    gfx = gfx_ptr;

    if (gfx->mode == GFX_VESA) {
        usize width = gfx->width / FONT_WIDTH;
        usize height = gfx->height / FONT_HEIGHT;

        term = term_init(width, height, _putc_vesa);
    } else {
        term = term_init(VGA_WIDTH, VGA_HEIGHT, _putc_vga);
    }
}
