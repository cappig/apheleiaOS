#include "color.h"

// Modified xterm color palette
// In little endian notation (0xAA|BBGGRR)
const u32 default_ansi_colors[16] = {
    0x000000,
    0x0000cd,
    0x00cd00,
    0x00cdcd,
    0xee0000,
    0xcd00cd,
    0xcdcd00,
    0xbfbfbf,
    0x7f7f7f,
    0x0000ff,
    0x00ff00,
    0x00ffff,
    0xff5c5c,
    0xff00ff,
    0xffff00,
    0xffffff,
};


// TODO: this kinda sucks. But it works for now
int color_palette_index(u32 color) {
    for (usize i = 0; i < 16; i++)
        if (default_ansi_colors[i] == color)
            return i;

    return -1;
}

rgba_color ansi_to_rgb(u8 index) {
    // Standars 16 colors
    if (index < 16)
        return (rgba_color){.raw = default_ansi_colors[index]};

    // Grayscale
    if (index > 231) {
        u8 c = (index - 232) * 10 + 8;
        return rgb_to_color(c, c, c);
    }

    // This is usually implemented as a LUT but this works as well
    // https://stackoverflow.com/a/27165165
    usize red_index = ((index - 16) / 36);
    u8 red = red_index > 0 ? 55 + red_index * 40 : 0;

    usize green_index = (((index - 16) % 36) / 6);
    u8 green = green_index > 0 ? 55 + green_index * 40 : 0;

    usize blue_index = ((index - 16) % 6);
    u8 blue = blue_index > 0 ? 55 + blue_index * 40 : 0;

    return rgb_to_color(red, green, blue);
}

int color_delta(rgba_color c1, rgba_color c2) {
    int delta_r = c1.r - c2.r;
    int delta_g = c1.g - c2.g;
    int delta_b = c1.b - c2.b;

    return delta_r * delta_r + delta_g * delta_g + delta_b * delta_b;
}
