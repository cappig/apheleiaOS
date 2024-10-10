#include "vesa.h"

#include <term/term.h>

#include "color.h"
#include "state.h"


void vesa_draw_pixel(graphics_state* graphics, usize x, usize y, rgba_color color) {
    u8* fb = (u8*)graphics->framebuffer;
    u32 offset = y * graphics->pitch + x * graphics->depth;

    fb[offset] = color.b; // blue
    fb[offset + 1] = color.g; // green
    fb[offset + 2] = color.r; // red
    // framebuffer[offset+3] = 0x00; // alpha
}
