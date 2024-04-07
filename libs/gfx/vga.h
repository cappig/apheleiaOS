#pragma once

#include "color.h"

#define VGA_ADDR   0xb8000
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

typedef enum {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_GREY = 7,
    VGA_LIGHT_BLACK = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW = 14,
    VGA_WHITE = 15,
} vga_colors;


int ansi_to_vga(int ansi_index);
int rgb_to_closest_vga(rgba_color color);
