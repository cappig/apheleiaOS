#include "vesa.h"

#include <term/term.h>

#include "font.h"
#include "state.h"


void vesa_draw_pixel(graphics_state* graphics, usize x, usize y, u32 color) {
    u8* fb = (u8*)graphics->framebuffer;
    u32 offset = y * graphics->pitch + x * graphics->depth;

    fb[offset] = color & 0xff; // blue
    fb[offset + 1] = (color >> 8) & 0xff; // green
    fb[offset + 2] = (color >> 16) & 0xff; // red
    // framebuffer[offset+3] = 0x00; // alpha
}

void vesa_putc(graphics_state* graphics, u32 x, u32 y, term_char ch) {
    const u8* glyph = font_bitmap[ch.ascii - 32];

    for (usize h = 0; h < FONT_HEIGHT; h++) {
        for (usize w = FONT_WIDTH; w > 0; w--) {
            u32 color;
            if (glyph[h] & (1 << w))
                color = ch.style.fg.raw;
            else
                color = ch.style.bg.raw;

            vesa_draw_pixel(graphics, x + (FONT_WIDTH - w), y + h, color);
        }
    }
}
