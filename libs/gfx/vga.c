#include "vga.h"

#include <base/types.h>
#include <limits.h>

#include "color.h"

static const u32 vga_palette[16] = {
    0x000000,
    0x0000aa,
    0x00aa00,
    0x00aaaa,
    0xaa0000,
    0xaa00aa,
    0xaa5500,
    0xaaaaaa,
    0x555555,
    0x5555ff,
    0x55ff55,
    0x55ffff,
    0xff5555,
    0xff55ff,
    0xffff55,
    0xffffff,
};


int ansi_to_vga(int ansi_index) {
    static const u8 lut[16] = {0, 4, 2, 6, 1, 5, 3, 7, 8, 12, 10, 14, 9, 13, 11, 15};

    return lut[ansi_index];
}

int rgb_to_closest_vga(rgba_color color) {
    int vga_color = 0;
    int smallest_delta = INT_MAX;

    for (usize i = 0; i < 16; i++) {
        int delta = color_delta((rgba_color)vga_palette[i], color);

        if (delta < smallest_delta) {
            vga_color = i;
            smallest_delta = delta;
        }
    }

    return vga_color;
}
