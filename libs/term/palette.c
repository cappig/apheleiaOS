
#include <base/types.h>
#include <gfx/color.h>
#include <gfx/vga.h>

#include "term.h"


static u8 _get_vga_color(rgba_color color) {
    int vga = ansi_to_vga(color_palette_index(color.raw));

    if (vga < 0)
        vga = rgb_to_closest_vga(color);

    return vga;
}


u16 term_char_to_vga(term_char ch) {
    u8 bg = _get_vga_color(ch.style.bg);
    u8 fg = _get_vga_color(ch.style.fg);

    if (ch.style.flags & TERM_FLAG_BOLD)
        if (fg < 8)
            fg += 8;

    if (ch.style.flags & TERM_FLAG_FAINT)
        if (fg >= 8)
            fg -= 8;

    return (bg << 4) | (fg & 0x0f);
}

void term_set_palette(terminal* term, const u32 palette[static 16]) {
    for (usize i = 0; i < 16; i++)
        term->palette[i].raw = palette[i];
}
