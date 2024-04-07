#include "color.h"

// modified xterm color palette
const u32 default_ansi_colors[16] = {
    0x000000,
    0xcd0000,
    0x00cd00,
    0xcdcd00,
    0x0000ee,
    0xcd00cd,
    0x00cdcd,
    0xa8a8a8,
    0x7f7f7f,
    0xff0000,
    0x00ff00,
    0xffff00,
    0x5c5cff,
    0xff00ff,
    0x00ffff,
    0xffffff,
};


// TODO: this kinda sucks. But it works for now
int color_palette_index(u32 color) {
    for (usize i = 0; i < 16; i++)
        if (default_ansi_colors[i] == color)
            return i;

    return -1;
}

int color_delta(rgba_color c1, rgba_color c2) {
    int delta_r = c1.r - c2.r;
    int delta_g = c1.g - c2.g;
    int delta_b = c1.b - c2.b;

    return delta_r * delta_r + delta_g * delta_g + delta_b * delta_b;
}
