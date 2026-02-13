#pragma once

#include <base/types.h>
#include <stddef.h>
#include <sys/font.h>
#include <x86/boot.h>

typedef enum {
    CONSOLE_DISABLED = 0,
    CONSOLE_VGA = 1,
    CONSOLE_VESA = 2,
} console_mode_t;

typedef struct {
    size_t cursor_x;
    size_t cursor_y;
    size_t saved_cursor_x;
    size_t saved_cursor_y;
    bool saved_cursor_valid;
    u8 vga_attr;
    u8 fg_vga;
    u8 bg_vga;
    u32 fg_rgb;
    u32 bg_rgb;
    bool bright;
    u8 utf8_pending[4];
    size_t utf8_pending_len;
} console_screen_t;

typedef struct {
    u32 codepoint;
    u8 fg;
    u8 bg;
} console_cell_t;

typedef struct {
    console_mode_t mode;
    u64 fb_phys;
    u8* fb;
    size_t fb_size;
    u8* fb_back;
    u32 width;
    u32 height;
    u32 pitch;
    u8 bytes_per_pixel;
    size_t cols;
    size_t rows;
    const font_t* font;
    u32 font_width;
    u32 font_height;
    u32 font_row_bytes;
    u32 font_glyph_bytes;
    bool use_phys_window;
    bool ready;
    size_t screen_count;
    size_t active_screen;
    console_screen_t* screens;
    console_cell_t* cells;
    console_screen_t fallback_screen;

    bool cursor_drawn;
    size_t cursor_draw_x;
    size_t cursor_draw_y;
    bool cursor_batch;

    bool flush_batch;
    bool dirty;
    size_t dirty_x0;
    size_t dirty_y0;
    size_t dirty_x1;
    size_t dirty_y1;
} console_state_t;

void console_init(const boot_info_t* info);
void console_set_font(const font_t* font);
