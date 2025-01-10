#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define ALPHA_ASCII                     \
    "\n"                                \
    " _________                     \n" \
    "  @-----@     The Apheleia     \n" \
    "   | | |    operating system   \n" \
    "   | | |                       \n" \
    "   | | |    (c) 2025 - GPLv3   \n" \
    "   | | |                     \n\n"

#define ALPHA_COLORS_DIM               \
    "\e[40m  \e[41m  \e[42m  \e[43m  " \
    "\e[44m  \e[45m  \e[46m  \e[47;0m"

#define ALPHA_COLORS_BRIGHT                \
    "\e[100m  \e[101m  \e[102m  \e[103m  " \
    "\e[104m  \e[105m  \e[106m  \e[107;0m"

typedef enum {
    GFX_NONE,
    GFX_VESA,
    GFX_VGA,
} graphics_mode;

typedef struct PACKED {
    u8 mode;

    u8 depth;
    u16 pitch;

    u16 width;
    u16 height;

    u16 monitor_width;
    u16 monitor_height;

    u64 framebuffer;

    u8 red_mask;
    u8 green_mask;
    u8 blue_mask;
} graphics_state;


void dump_gfx_info(graphics_state* gfx);
