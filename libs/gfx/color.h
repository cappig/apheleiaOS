#pragma once

#include <base/attributes.h>
#include <base/types.h>

typedef union PACKED {
    struct PACKED {
        u8 r;
        u8 g;
        u8 b;
        u8 a;
    };
    u32 raw; // NOTE : beware of endianness
} rgba_color;

typedef enum {
    ANSI_BLACK = 0,
    ANSI_RED = 1,
    ANSI_GREEN = 2,
    ANSI_YELLOW = 3,
    ANSI_BLUE = 4,
    ANSI_MAGENTA = 5,
    ANSI_CYAN = 6,
    ANSI_GREY = 7,
    ANSI_BRIGHT_BLACK = 8,
    ANSI_BRIGHT_RED = 9,
    ANSI_BRIGHT_GREEN = 10,
    ANSI_BRIGHT_YELLOW = 11,
    ANSI_BRIGHT_BLUE = 12,
    ANSI_BRIGHT_MAGENTA = 13,
    ANSI_BRIGHT_CYAN = 14,
    ANSI_BRIGHT_GREY = 15,
} ansi_colors;

inline rgba_color rgb_to_color(u8 red, u8 green, u8 blue) {
    return (rgba_color){.r = red, .g = green, .b = blue};
}

extern const u32 default_ansi_colors[16];


int color_palette_index(u32 color);

rgba_color ansi_to_rgb(u8 index);

int color_delta(rgba_color c1, rgba_color c2);
