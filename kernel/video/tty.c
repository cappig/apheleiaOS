#include "tty.h"

#include <base/types.h>
#include <boot/proto.h>
#include <fs/ustar.h>
#include <gfx/font.h>
#include <gfx/psf.h>
#include <gfx/state.h>
#include <gfx/vesa.h>
#include <gfx/vga.h>
#include <log/log.h>
#include <string.h>
#include <term/palette.h>
#include <term/term.h>
#include <x86/serial.h>

#include "arch/panic.h"
#include "drivers/initrd.h"

static kernel_console console = {0};


static void _putc_vga(term_char ch, usize index) {
    static volatile u16* vga_base = (u16*)ID_MAPPED_VADDR(VGA_ADDR);

    u16 attr = term_char_to_vga(ch);
    vga_base[index] = ch.ascii | (attr << 8);
}


static void _draw_vesa_psf(graphics_state* graphics, u32 x, u32 y, term_char ch) {
    const u8* glyph = (unsigned char*)console.font.data + ch.ascii * console.font.glyph_size;

    usize line_width = DIV_ROUND_UP(console.font.glyph_width, 8);
    for (usize h = 0; h < console.font.glyph_height; h++) {

        usize mask = 1 << console.font.glyph_width;
        for (usize w = 0; w < console.font.glyph_width; w++) {
            rgba_color color;
            if (*glyph & mask)
                color = ch.style.fg;
            else
                color = ch.style.bg;

            vesa_draw_pixel(graphics, x + w, y + h, color);

            mask >>= 1;
        }

        glyph += line_width;
    }
}

static void _putc_vesa_psf(term_char ch, usize index) {
    usize x = index % console.term->width;
    usize y = index / console.term->width;

    _draw_vesa_psf(&console.gfx, x * console.font.glyph_width, y * console.font.glyph_height, ch);
}


static void _draw_vesa_header(graphics_state* graphics, u32 x, u32 y, term_char ch) {
    const u8* glyph = header_font_bitmap[ch.ascii - 32];

    for (usize h = 0; h < HEADER_FONT_HEIGHT; h++) {
        for (usize w = HEADER_FONT_WIDTH; w > 0; w--) {
            rgba_color color;
            if (glyph[h] & (1 << w))
                color = ch.style.fg;
            else
                color = ch.style.bg;

            vesa_draw_pixel(graphics, x + (HEADER_FONT_WIDTH - w), y + h, color);
        }
    }
}

static void _putc_vesa_header(term_char ch, usize index) {
    usize x = index % console.term->width;
    usize y = index / console.term->width;

    _draw_vesa_header(&console.gfx, x * HEADER_FONT_WIDTH, y * HEADER_FONT_HEIGHT, ch);
}


static bool _load_psf(void* bin) {
    psf_font* font = &console.font;

    if (!bin)
        goto bitmap_fallback;

    psf1_header* psf1_head = bin;
    psf2_header* psf2_head = bin;

    if (psf2_head->magic == PSF2_MAGIC) {
        font->type = PSF_FONT_PSF2;
        font->data = bin + sizeof(psf2_header);

        font->glyph_height = psf2_head->height;
        font->glyph_width = psf2_head->width;
        font->glyph_size = psf2_head->glyph_bytes;
    }

    else if (psf1_head->magic == PSF1_MAGIC) {
        font->type = PSF_FONT_PSF1;
        font->data = bin + sizeof(psf1_header);

        font->glyph_height = psf1_head->char_size;
        font->glyph_width = PSF1_WIDTH;
        font->glyph_size = psf1_head->char_size;
    }

    else {
    bitmap_fallback:
        font->type = PSF_FONT_NONE;

        font->glyph_height = HEADER_FONT_HEIGHT;
        font->glyph_width = HEADER_FONT_WIDTH;

        return false;
    }

    font->header = bin;

    return true;
}


void puts(const char* s) {
    send_serial_string(SERIAL_COM1, s);

    if (console.term)
        term_parse(console.term, s, (usize)-1);
}


void tty_init(graphics_state* gfx_ptr, boot_handoff* handoff) {
    memcpy(&console.gfx, gfx_ptr, sizeof(graphics_state));

    bool is_psf = false;
    if (console.gfx.mode == GFX_VESA) {

        void* psf_font = initd_find(handoff->args.console_font);
        is_psf = _load_psf(psf_font);

        usize width = console.gfx.width / console.font.glyph_width;
        usize height = console.gfx.height / console.font.glyph_height;

        console.gfx.framebuffer = ID_MAPPED_VADDR(console.gfx.framebuffer);

        term_putc_fn term_fn = is_psf ? _putc_vesa_psf : _putc_vesa_header;
        console.term = term_init(width, height, term_fn);
    } else {
        console.term = term_init(VGA_WIDTH, VGA_HEIGHT, _putc_vga);
    }

    if (!console.term)
        panic("Failed to initialize terminal!");

    if (!is_psf)
        log_warn("Initialized kernel console with fallback header font.");
}
