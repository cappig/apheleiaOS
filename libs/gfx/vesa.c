#include "vesa.h"

#include <term/term.h>

#include "color.h"
#include "font.h"
#include "state.h"


void vesa_draw_pixel(graphics_state* graphics, usize x, usize y, rgba_color color) {
    u8* fb = (u8*)graphics->framebuffer;
    u32 offset = y * graphics->pitch + x * graphics->depth;

    fb[offset] = color.b; // blue
    fb[offset + 1] = color.g; // green
    fb[offset + 2] = color.r; // red
    // framebuffer[offset+3] = 0x00; // alpha
}

void vesa_putc(graphics_state* graphics, u32 x, u32 y, term_char ch) {
    const u8* glyph = font_bitmap[ch.ascii - 32];

    for (usize h = 0; h < FONT_HEIGHT; h++) {
        for (usize w = FONT_WIDTH; w > 0; w--) {
            rgba_color color;
            if (glyph[h] & (1 << w))
                color = ch.style.fg;
            else
                color = ch.style.bg;

            vesa_draw_pixel(graphics, x + (FONT_WIDTH - w), y + h, color);
        }
    }
}
