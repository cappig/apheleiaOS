#include "tty.h"

#include <base/addr.h>
#include <base/types.h>
#include <boot/proto.h>
#include <fs/ustar.h>
#include <gfx/font.h>
#include <gfx/psf.h>
#include <gfx/state.h>
#include <gfx/vga.h>
#include <log/log.h>
#include <string.h>
#include <term/palette.h>
#include <term/term.h>
#include <x86/serial.h>

#include "arch/panic.h"
#include "drivers/initrd.h"
#include "drivers/vesa.h"

static graphics_state gfx = {0};
static psf_font font = {0};
static terminal* term;


static void _putc_vga(term_char ch, usize index) {
    static volatile u16* vga_base = (u16*)ID_MAPPED_VADDR(VGA_ADDR);

    u16 attr = term_char_to_vga(ch);
    vga_base[index] = ch.ascii | (attr << 8);
}


static void _draw_vesa_psf(graphics_state* graphics, u32 x, u32 y, term_char ch) {
    const u8* glyph = (unsigned char*)font.data + ch.ascii * font.glyph_size;

    usize line_width = DIV_ROUND_UP(font.glyph_width, 8);
    for (usize h = 0; h < font.glyph_height; h++) {

        usize mask = 1 << font.glyph_width;
        for (usize w = 0; w < font.glyph_width; w++) {
            rgba_color color;
            if (*glyph & mask)
                color = ch.style.fg;
            else
                color = ch.style.bg;

            u32 vesa_color = to_vesa_color(graphics, color);
            vesa_draw_pixel(graphics, x + w, y + h, vesa_color);

            mask >>= 1;
        }

        glyph += line_width;
    }
}

static void _putc_vesa_psf(term_char ch, usize index) {
    usize x = index % term->width;
    usize y = index / term->width;

    _draw_vesa_psf(&gfx, x * font.glyph_width, y * font.glyph_height, ch);
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

            u32 vesa_color = to_vesa_color(graphics, color);
            vesa_draw_pixel(graphics, x + (HEADER_FONT_WIDTH - w), y + h, vesa_color);
        }
    }
}

static void _putc_vesa_header(term_char ch, usize index) {
    usize x = index % term->width;
    usize y = index / term->width;

    _draw_vesa_header(&gfx, x * HEADER_FONT_WIDTH, y * HEADER_FONT_HEIGHT, ch);
}


static bool _load_psf(void* bin) {
    if (!bin)
        goto bitmap_fallback;

    psf1_header* psf1_head = bin;
    psf2_header* psf2_head = bin;

    if (psf2_head->magic == PSF2_MAGIC) {
        font.type = PSF_FONT_PSF2;
        font.data = bin + sizeof(psf2_header);

        font.glyph_height = psf2_head->height;
        font.glyph_width = psf2_head->width;
        font.glyph_size = psf2_head->glyph_bytes;
    }

    else if (psf1_head->magic == PSF1_MAGIC) {
        font.type = PSF_FONT_PSF1;
        font.data = bin + sizeof(psf1_header);

        font.glyph_height = psf1_head->char_size;
        font.glyph_width = PSF1_WIDTH;
        font.glyph_size = psf1_head->char_size;
    }

    else {
    bitmap_fallback:
        font.type = PSF_FONT_NONE;

        font.glyph_height = HEADER_FONT_HEIGHT;
        font.glyph_width = HEADER_FONT_WIDTH;

        return false;
    }

    font.header = bin;

    return true;
}


terminal* tty_init(graphics_state* gfx_ptr, boot_handoff* handoff) {
    memcpy(&gfx, gfx_ptr, sizeof(graphics_state));

    bool is_psf = false;
    if (gfx.mode == GFX_VESA) {
        void* font_file = initd_find(handoff->args.console_font);
        is_psf = _load_psf(font_file);

        usize width = gfx.width / font.glyph_width;
        usize height = gfx.height / font.glyph_height;

        term_putc_fn term_fn = is_psf ? _putc_vesa_psf : _putc_vesa_header;
        term = term_init(width, height, term_fn);
    } else {
        term = term_init(VGA_WIDTH, VGA_HEIGHT, _putc_vga);
    }

    if (!term)
        panic("Failed to initialize terminal!");

    if (!is_psf && gfx_ptr->mode == GFX_VESA)
        log_warn("Initialized kernel console with fallback header font.");

    return term;
}


void dump_gfx_info(graphics_state* gfx_ptr) {
    char* mode_str;

    switch (gfx_ptr->mode) {
    case GFX_VESA:
        mode_str = "VESA graphics";
        break;
    case GFX_VGA:
        mode_str = "VGA text";
        break;
    default:
        mode_str = "headless";
        break;
    }

    log_info(
        "Running in %s mode with a resolution of %dx%d:%d",
        mode_str,
        gfx_ptr->width,
        gfx_ptr->height,
        gfx_ptr->depth * 8
    );
}
