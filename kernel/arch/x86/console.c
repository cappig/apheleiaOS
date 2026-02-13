#include "console.h"

#include <arch/arch.h>
#include <base/macros.h>
#include <base/types.h>
#include <base/utf8.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/font.h>
#include <sys/tty.h>
#include <x86/serial.h>
#include <x86/vga.h>

static console_state_t console_state = {0};

static const u32 ansi_rgb[16] = {
    0x000000,
    0x800000,
    0x008000,
    0x808000,
    0x000080,
    0x800080,
    0x008080,
    0xc0c0c0,
    0x808080,
    0xff0000,
    0x00ff00,
    0xffff00,
    0x0000ff,
    0xff00ff,
    0x00ffff,
    0xffffff,
};


static void _screen_update_vga_attr(console_screen_t* screen) {
    if (!screen)
        return;

    screen->vga_attr = (u8)((screen->bg_vga << 4) | (screen->fg_vga & 0x0f));
}

static void _screen_reset_colors(console_screen_t* screen) {
    if (!screen)
        return;

    screen->bright = false;
    screen->fg_vga = VGA_GREY;
    screen->bg_vga = VGA_BLACK;
    screen->fg_rgb = ansi_rgb[screen->fg_vga];
    screen->bg_rgb = ansi_rgb[screen->bg_vga];

    _screen_update_vga_attr(screen);
}

static void _screen_reset(console_screen_t* screen) {
    if (!screen)
        return;

    console_screen_reset_colors(screen);

    screen->cursor_x = 0;
    screen->cursor_y = 0;
    screen->saved_cursor_x = 0;
    screen->saved_cursor_y = 0;
    screen->saved_cursor_valid = false;
    screen->utf8_pending_len = 0;
}

static size_t _cell_count(void) {
    return console_state.cols * console_state.rows;
}

static console_screen_t* _get_screen(size_t index) {
    if (index >= console_state.screen_count)
        return NULL;

    if (!console_state.screens)
        return NULL;

    return &console_state.screens[index];
}

static console_cell_t* _screen_cells(size_t index) {
    if (!console_state.cells)
        return NULL;

    size_t count = _cell_count();
    return console_state.cells + index * count;
}

static void _clear_screen_buffer(size_t index) {
    console_screen_t* screen = _get_screen(index);
    console_cell_t* cells = _screen_cells(index);

    if (!screen || !cells)
        return;

    size_t count = _cell_count();

    for (size_t i = 0; i < count; i++) {
        cells[i].codepoint = ' ';
        cells[i].fg = screen->fg_vga;
        cells[i].bg = screen->bg_vga;
    }
}

static void _update_vga_cursor(size_t col, size_t row) {
    if (!console_state.cols || !console_state.rows)
        return;

    if (col >= console_state.cols)
        col = console_state.cols - 1;
    if (row >= console_state.rows)
        row = console_state.rows - 1;

    u16 pos = (u16)(row * console_state.cols + col);

    outb(0x3d4, 0x0f);
    outb(0x3d5, (u8)(pos & 0xff));
    outb(0x3d4, 0x0e);
    outb(0x3d5, (u8)((pos >> 8) & 0xff));
}

static void _use_font(const font_t* font) {
    if (!font || !font->glyphs || !font->glyph_width || !font->glyph_height)
        return;

    console_state.font = font;
    console_state.font_width = font->glyph_width;
    console_state.font_height = font->glyph_height;
    console_state.font_row_bytes = font_row_bytes(font);
    console_state.font_glyph_bytes = font_glyph_bytes(font);
}

static u32 _font_index(u32 codepoint) {
    const font_t* font = console_state.font;
    if (!font)
        return 0;

    if (font->map && font->map_count) {
        for (u32 i = 0; i < font->map_count; i++) {
            if (font->map[i].codepoint == codepoint)
                return font->map[i].glyph;
        }
    }

    if (codepoint < font->first_char)
        return 0;

    u32 idx = codepoint - font->first_char;
    if (idx >= font->glyph_count)
        return 0;

    return idx;
}

static u8* _map_range(size_t offset, size_t size) {
    if (!size)
        return NULL;

#if defined(__i386__)
    if (console_state.use_phys_window)
        return arch_phys_map(console_state.fb_phys + (u64)offset, size);
#endif

    if (!console_state.fb)
        return NULL;

    return console_state.fb + offset;
}

static void _unmap_range(void* ptr, size_t size) {
#if defined(__i386__)
    if (console_state.use_phys_window)
        arch_phys_unmap(ptr, size);
#else
    (void)ptr;
    (void)size;
#endif
}

static bool _has_back_buffer(void) {
    return console_state.mode == CONSOLE_VESA && console_state.fb_back && console_state.fb_size > 0;
}

static void _mark_dirty_rect(size_t x, size_t y, size_t width, size_t height) {
    if (!_has_back_buffer() || !width || !height)
        return;

    if (x >= console_state.width || y >= console_state.height)
        return;

    if (x + width > console_state.width)
        width = console_state.width - x;
    if (y + height > console_state.height)
        height = console_state.height - y;

    size_t x1 = x + width;
    size_t y1 = y + height;

    if (!console_state.dirty) {
        console_state.dirty = true;
        console_state.dirty_x0 = x;
        console_state.dirty_y0 = y;
        console_state.dirty_x1 = x1;
        console_state.dirty_y1 = y1;
        return;
    }

    if (x < console_state.dirty_x0)
        console_state.dirty_x0 = x;

    if (y < console_state.dirty_y0)
        console_state.dirty_y0 = y;

    if (x1 > console_state.dirty_x1)
        console_state.dirty_x1 = x1;

    if (y1 > console_state.dirty_y1)
        console_state.dirty_y1 = y1;
}

static void _flush_dirty(void) {
    if (!_has_back_buffer() || !console_state.dirty)
        return;

    size_t x = console_state.dirty_x0;
    size_t y = console_state.dirty_y0;
    size_t width = console_state.dirty_x1 - console_state.dirty_x0;
    size_t height = console_state.dirty_y1 - console_state.dirty_y0;

    if (!width || !height) {
        console_state.dirty = false;
        return;
    }

    size_t width_bytes = width * console_state.bytes_per_pixel;
    size_t offset = y * console_state.pitch + x * console_state.bytes_per_pixel;
    size_t map_size = (height - 1) * console_state.pitch + width_bytes;

    u8* fb = _map_range(offset, map_size);
    if (!fb)
        return;

    const u8* src = console_state.fb_back + offset;

    for (size_t row = 0; row < height; row++)
        memcpy(fb + row * console_state.pitch, src + row * console_state.pitch, width_bytes);

    console_unmap_range(fb, map_size);
    console_state.dirty = false;
}

static void _maybe_flush_dirty(void) {
    if (console_state.flush_batch)
        return;

    _flush_dirty();
}

static void _write_pixel(u8* dst, u32 color) {
    switch (console_state.bytes_per_pixel) {
    case 4:
        *(u32*)dst = color;
        break;
    case 3:
        dst[0] = (u8)(color & 0xff);
        dst[1] = (u8)((color >> 8) & 0xff);
        dst[2] = (u8)((color >> 16) & 0xff);
        break;
    case 2:
        u8 r = (u8)((color >> 16) & 0xff);
        u8 g = (u8)((color >> 8) & 0xff);
        u8 b = (u8)(color & 0xff);
        u16 rgb565 = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        *(u16*)dst = rgb565;
        break;
    default:
        break;
    }
}

static void _fill_rect(size_t x, size_t y, size_t width, size_t height, u32 color) {
    if (!console_state.fb_size || !width || !height)
        return;

    if (x >= console_state.width || y >= console_state.height)
        return;

    if (x + width > console_state.width)
        width = console_state.width - x;
    if (y + height > console_state.height)
        height = console_state.height - y;

    if (_has_back_buffer()) {
        size_t offset = y * console_state.pitch + x * console_state.bytes_per_pixel;
        u8* base = console_state.fb_back + offset;

        for (size_t row = 0; row < height; row++) {
            u8* row_base = base + row * console_state.pitch;

            for (size_t col = 0; col < width; col++)
                _write_pixel(row_base + col * console_state.bytes_per_pixel, color);
        }

        console_mark_dirty_rect(x, y, width, height);
        console_maybe_flush_dirty();

        return;
    }

    size_t width_bytes = width * console_state.bytes_per_pixel;
    size_t map_size = (height - 1) * console_state.pitch + width_bytes;
    size_t offset = y * console_state.pitch + x * console_state.bytes_per_pixel;

    u8* base = _map_range(offset, map_size);
    if (!base)
        return;

    for (size_t row = 0; row < height; row++) {
        u8* row_base = base + row * console_state.pitch;

        for (size_t col = 0; col < width; col++) {
            _write_pixel(row_base + col * console_state.bytes_per_pixel, color);
        }
    }

    console_unmap_range(base, map_size);
}

static void _clear_vga(const console_screen_t* screen) {
    if (!console_state.fb || !screen)
        return;

    u16* vga = (u16*)console_state.fb;
    u16 entry = ((u16)screen->vga_attr << 8) | ' ';

    for (size_t i = 0; i < console_state.cols * console_state.rows; i++)
        vga[i] = entry;
}

static void _clear_vesa(const console_screen_t* screen) {
    if (!screen)
        return;

    _fill_rect(0, 0, console_state.width, console_state.height, screen->bg_rgb);
}

static void _scroll_vga(const console_screen_t* screen) {
    if (!console_state.fb || !screen)
        return;

    u16* vga = (u16*)console_state.fb;
    size_t row_len = console_state.cols;
    size_t rows = console_state.rows;

    memmove(vga, vga + row_len, (rows - 1) * row_len * sizeof(u16));

    u16 entry = ((u16)screen->vga_attr << 8) | ' ';

    for (size_t i = (rows - 1) * row_len; i < rows * row_len; i++)
        vga[i] = entry;
}

static void _scroll_vesa(const console_screen_t* screen) {
    if (!screen || !console_state.fb_size || !console_state.font_height)
        return;

    size_t line_bytes = console_state.pitch * console_state.font_height;
    size_t move_bytes = console_state.pitch * (console_state.height - console_state.font_height);

    if (_has_back_buffer()) {
        memmove(console_state.fb_back, console_state.fb_back + line_bytes, move_bytes);
        _mark_dirty_rect(0, 0, console_state.width, console_state.height);
    } else {
        u8* fb = _map_range(0, console_state.fb_size);
        if (!fb)
            return;

        memmove(fb, fb + line_bytes, move_bytes);
        console_unmap_range(fb, console_state.fb_size);
    }

    _fill_rect(
        0,
        console_state.height - console_state.font_height,
        console_state.width,
        console_state.font_height,
        screen->bg_rgb
    );

    _maybe_flush_dirty();
}

static void _draw_char_vesa(u32 codepoint, size_t col, size_t row, u32 fg_rgb, u32 bg_rgb) {
    if (!console_state.fb_size || !console_state.font || !console_state.font_width ||
        !console_state.font_height)
        return;

    size_t x = col * console_state.font_width;
    size_t y = row * console_state.font_height;
    size_t width_bytes = console_state.font_width * console_state.bytes_per_pixel;
    size_t map_size = (console_state.font_height - 1) * console_state.pitch + width_bytes;

    u32 index = _font_index(codepoint);
    const u8* glyph = console_state.font->glyphs + index * console_state.font_glyph_bytes;

    size_t offset = y * console_state.pitch + x * console_state.bytes_per_pixel;
    u8* base = NULL;
    if (_has_back_buffer()) {
        base = console_state.fb_back + offset;
    } else {
        base = _map_range(offset, map_size);

        if (!base)
            return;
    }

    for (size_t gy = 0; gy < console_state.font_height; gy++) {
        const u8* row_ptr = glyph + gy * console_state.font_row_bytes;
        u8* row_base = base + gy * console_state.pitch;

        for (size_t gx = 0; gx < console_state.font_width; gx++) {
            u8 mask = (u8)(0x80 >> (gx & 7));
            u8 bits = row_ptr[gx / 8];
            u32 color = (bits & mask) ? fg_rgb : bg_rgb;

            console_write_pixel(row_base + gx * console_state.bytes_per_pixel, color);
        }
    }

    if (_has_back_buffer()) {
        _mark_dirty_rect(x, y, console_state.font_width, console_state.font_height);
        _maybe_flush_dirty();
    } else {
        console_unmap_range(base, map_size);
    }
}

static void _draw_char_vga(u32 codepoint, size_t col, size_t row, u8 attr) {
    if (!console_state.fb)
        return;

    u16* vga = (u16*)console_state.fb;
    size_t index = row * console_state.cols + col;
    u8 ch = (codepoint > 0xff) ? (u8)'?' : (u8)codepoint;
    u16 entry = ((u16)attr << 8) | ch;
    vga[index] = entry;
}


static void _cursor_hide(void) {
    if (!console_state.cursor_drawn)
        return;

    if (console_state.mode == CONSOLE_VGA) {
        console_state.cursor_drawn = false;
        return;
    }

    if (console_state.mode != CONSOLE_VESA)
        return;

    console_screen_t* screen = console_get_screen(console_state.active_screen);
    console_cell_t* cells = console_screen_cells(console_state.active_screen);

    if (!screen || !cells)
        return;

    size_t col = console_state.cursor_draw_x;
    size_t row = console_state.cursor_draw_y;

    if (col >= console_state.cols || row >= console_state.rows)
        return;

    console_cell_t* cell = &cells[row * console_state.cols + col];
    u32 codepoint = cell->codepoint ? cell->codepoint : ' ';
    u32 fg = ansi_rgb[cell->fg & 0x0f];
    u32 bg = ansi_rgb[cell->bg & 0x0f];

    console_draw_char_vesa(codepoint, col, row, fg, bg);
    console_state.cursor_drawn = false;
}
static void _cursor_show(size_t screen_index) {
    if (console_state.cursor_batch)
        return;

    if (screen_index != console_state.active_screen)
        return;

    console_screen_t* screen = console_get_screen(screen_index);
    console_cell_t* cells = console_screen_cells(screen_index);

    if (!screen || !cells || !console_state.cols || !console_state.rows)
        return;

    if (console_state.mode == CONSOLE_VGA) {
        console_update_vga_cursor(screen->cursor_x, screen->cursor_y);
        console_state.cursor_drawn = true;
        console_state.cursor_draw_x = screen->cursor_x;
        console_state.cursor_draw_y = screen->cursor_y;
        return;
    }

    if (console_state.mode != CONSOLE_VESA)
        return;

    _cursor_hide();

    size_t col = screen->cursor_x;
    size_t row = screen->cursor_y;
    if (col >= console_state.cols || row >= console_state.rows)
        return;

    console_cell_t* cell = &cells[row * console_state.cols + col];
    u32 codepoint = cell->codepoint ? cell->codepoint : ' ';
    u32 fg = ansi_rgb[cell->bg & 0x0f];
    u32 bg = ansi_rgb[cell->fg & 0x0f];

    console_draw_char_vesa(codepoint, col, row, fg, bg);

    console_state.cursor_drawn = true;
    console_state.cursor_draw_x = col;
    console_state.cursor_draw_y = row;
}
static void _clear_screen_range(size_t index, size_t start, size_t end) {
    console_screen_t* screen = console_get_screen(index);
    console_cell_t* cells = console_screen_cells(index);

    if (!screen)
        return;

    size_t count = console_cell_count();
    if (!count || start >= count)
        return;

    if (end > count)
        end = count;

    bool full_clear = !start && end == count;

    if (cells) {
        for (size_t i = start; i < end; i++) {
            cells[i].codepoint = ' ';
            cells[i].fg = screen->fg_vga;
            cells[i].bg = screen->bg_vga;
        }
    }

    if (index != console_state.active_screen)
        return;

    bool temp_batch = false;
    if (console_has_back_buffer() && !console_state.flush_batch) {
        console_state.flush_batch = true;
        temp_batch = true;
    }

    _cursor_hide();

    if (full_clear) {
        if (console_state.mode == CONSOLE_VGA)
            console_clear_vga(screen);
        else if (console_state.mode == CONSOLE_VESA)
            console_clear_vesa(screen);

        console_state.cursor_drawn = false;
        if (temp_batch) {
            console_state.flush_batch = false;
            console_flush_dirty();
        }
        return;
    }

    if (console_state.mode == CONSOLE_VGA) {
        for (size_t i = start; i < end; i++) {
            size_t row = i / console_state.cols;
            size_t col = i % console_state.cols;
            console_draw_char_vga(' ', col, row, screen->vga_attr);
        }
    } else if (console_state.mode == CONSOLE_VESA) {
        for (size_t i = start; i < end; i++) {
            size_t row = i / console_state.cols;
            size_t col = i % console_state.cols;
            console_draw_char_vesa(' ', col, row, screen->fg_rgb, screen->bg_rgb);
        }
    }

    console_state.cursor_drawn = false;
    if (temp_batch) {
        console_state.flush_batch = false;
        console_flush_dirty();
    }
}
static void _handle_csi_clear(size_t index, console_screen_t* screen, int mode) {
    size_t cols = console_state.cols;
    size_t rows = console_state.rows;
    size_t count = console_cell_count();

    if (!screen || !cols || !rows || !count)
        return;

    size_t cursor = screen->cursor_y * cols + screen->cursor_x;

    switch (mode) {
    case 0:
        _clear_screen_range(index, cursor, count);
        break;
    case 1:
        _clear_screen_range(index, 0, cursor + 1);
        break;
    case 2:
        _clear_screen_range(index, 0, count);
        break;
    default:
        break;
    }

    _cursor_show(index);
}
static void _handle_csi_clear_line(size_t index, console_screen_t* screen, int mode) {
    if (!screen || !console_state.cols || !console_state.rows)
        return;

    size_t row = screen->cursor_y;
    if (row >= console_state.rows)
        row = console_state.rows - 1;

    size_t row_start = row * console_state.cols;
    size_t row_end = row_start + console_state.cols;
    size_t cursor = row_start + screen->cursor_x;

    if (cursor >= row_end)
        cursor = row_end - 1;

    switch (mode) {
    case 0:
        _clear_screen_range(index, cursor, row_end);
        break;
    case 1:
        _clear_screen_range(index, row_start, cursor + 1);
        break;
    case 2:
        _clear_screen_range(index, row_start, row_end);
        break;
    default:
        break;
    }

    _cursor_show(index);
}
static void _handle_csi_cursor(size_t index, console_screen_t* screen, int row, int col) {
    if (!screen)
        return;

    if (row <= 0)
        row = 1;

    if (col <= 0)
        col = 1;

    if ((size_t)row > console_state.rows)
        row = (int)console_state.rows;

    if ((size_t)col > console_state.cols)
        col = (int)console_state.cols;

    screen->cursor_y = (size_t)(row - 1);
    screen->cursor_x = (size_t)(col - 1);

    _cursor_show(index);
}
static void
_handle_csi_move(size_t index, console_screen_t* screen, int row_delta, int col_delta) {
    if (!screen || !console_state.cols || !console_state.rows)
        return;

    int row = (int)screen->cursor_y + row_delta;
    int col = (int)screen->cursor_x + col_delta;

    if (row < 0)
        row = 0;

    if (col < 0)
        col = 0;

    if ((size_t)row >= console_state.rows)
        row = (int)console_state.rows - 1;

    if ((size_t)col >= console_state.cols)
        col = (int)console_state.cols - 1;

    screen->cursor_y = (size_t)row;
    screen->cursor_x = (size_t)col;
    _cursor_show(index);
}
static void _redraw_screen(size_t index) {
    console_screen_t* screen = console_get_screen(index);
    console_cell_t* cells = console_screen_cells(index);

    if (!screen || !console_state.ready)
        return;

    bool temp_batch = false;

    if (index == console_state.active_screen && console_has_back_buffer() &&
        !console_state.flush_batch) {
        console_state.flush_batch = true;
        temp_batch = true;
    }

    if (console_state.mode == CONSOLE_VGA)
        console_clear_vga(screen);
    else if (console_state.mode == CONSOLE_VESA)
        console_clear_vesa(screen);

    if (!cells)
        return;

    size_t cols = console_state.cols;
    size_t rows = console_state.rows;

    for (size_t row = 0; row < rows; row++) {
        for (size_t col = 0; col < cols; col++) {
            console_cell_t* cell = &cells[row * cols + col];
            u32 codepoint = cell->codepoint ? cell->codepoint : ' ';

            if (console_state.mode == CONSOLE_VGA) {
                u8 attr = (u8)((cell->bg << 4) | (cell->fg & 0x0f));
                console_draw_char_vga(codepoint, col, row, attr);
            } else if (console_state.mode == CONSOLE_VESA) {
                u32 fg = ansi_rgb[cell->fg & 0x0f];
                u32 bg = ansi_rgb[cell->bg & 0x0f];
                console_draw_char_vesa(codepoint, col, row, fg, bg);
            }
        }
    }

    if (temp_batch) {
        console_state.flush_batch = false;
        console_flush_dirty();
    }

    _cursor_show(index);
}
static bool _set_active(size_t index) {
    if (!console_state.ready)
        return false;

    if (index >= console_state.screen_count)
        return false;

    if (console_state.active_screen == index)
        return true;

    console_state.active_screen = index;
    _redraw_screen(index);
    return true;
}
static void _set_fg(console_screen_t* screen, u8 base, bool force_bright) {
    if (!screen)
        return;

    u8 idx = base & 0x7;
    if (force_bright || screen->bright)
        idx = (u8)(idx + 8);

    screen->fg_vga = idx;
    screen->fg_rgb = ansi_rgb[idx];

    console_screen_update_vga_attr(screen);
}
static void _set_bg(console_screen_t* screen, u8 base, bool bright) {
    if (!screen)
        return;

    u8 idx = base & 0x7;
    if (bright)
        idx = (u8)(idx + 8);

    screen->bg_vga = idx;
    screen->bg_rgb = ansi_rgb[idx];

    console_screen_update_vga_attr(screen);
}
static void _apply_sgr(console_screen_t* screen, int code) {
    if (!screen)
        return;

    if (!code) {
        console_screen_reset_colors(screen);
        return;
    }

    if (code == 1) {
        screen->bright = true;
        _set_fg(screen, screen->fg_vga & 0x7, true);
        return;
    }

    if (code == 2 || code == 22) {
        screen->bright = false;
        _set_fg(screen, screen->fg_vga & 0x7, false);
        return;
    }

    if (code == 39) {
        _set_fg(screen, VGA_GREY, false);
        return;
    }

    if (code == 49) {
        _set_bg(screen, VGA_BLACK, false);
        return;
    }

    if (code >= 30 && code <= 37) {
        _set_fg(screen, (u8)(code - 30), false);
        return;
    }

    if (code >= 90 && code <= 97) {
        _set_fg(screen, (u8)(code - 90), true);
        return;
    }

    if (code >= 40 && code <= 47) {
        _set_bg(screen, (u8)(code - 40), false);
        return;
    }

    if (code >= 100 && code <= 107)
        _set_bg(screen, (u8)(code - 100), true);
}

static void _scroll_screen(console_screen_t* screen, size_t screen_index) {
    if (!screen)
        return;

    console_cell_t* cells = _screen_cells(screen_index);
    size_t cols = console_state.cols;
    size_t rows = console_state.rows;

    if (cells && cols && rows) {
        memmove(cells, cells + cols, (rows - 1) * cols * sizeof(*cells));

        console_cell_t* last = cells + (rows - 1) * cols;

        for (size_t i = 0; i < cols; i++) {
            last[i].codepoint = ' ';
            last[i].fg = screen->fg_vga;
            last[i].bg = screen->bg_vga;
        }
    }

    if (screen_index != console_state.active_screen)
        return;

    if (console_state.mode == CONSOLE_VGA)
        _scroll_vga(screen);
    else if (console_state.mode == CONSOLE_VESA)
        _scroll_vesa(screen);
}

static void _newline(console_screen_t* screen, size_t screen_index) {
    screen->cursor_x = 0;
    screen->cursor_y++;

    if (screen->cursor_y < console_state.rows)
        return;

    screen->cursor_y = console_state.rows - 1;
    _scroll_screen(screen, screen_index);
}

static void _putc(console_screen_t* screen, size_t screen_index, u32 ch) {
    if (!screen || !console_state.ready || console_state.mode == CONSOLE_DISABLED)
        return;

    if (ch == '\n') {
        _newline(screen, screen_index);
        return;
    }

    if (ch == '\r') {
        screen->cursor_x = 0;
        return;
    }

    if (ch == '\t') {
        size_t next = (screen->cursor_x + 4) & ~3ULL;

        while (screen->cursor_x < next)
            _putc(screen, screen_index, ' ');

        return;
    }

    if (ch == '\b') {
        if (screen->cursor_x > 0) {
            screen->cursor_x--;

            size_t col = screen->cursor_x;
            size_t row = screen->cursor_y;

            console_cell_t* cells = console_screen_cells(screen_index);

            if (cells) {
                console_cell_t* cell = &cells[row * console_state.cols + col];
                cell->codepoint = ' ';
                cell->fg = screen->fg_vga;
                cell->bg = screen->bg_vga;
            }

            if (screen_index == console_state.active_screen) {
                if (console_state.mode == CONSOLE_VGA) {
                    u8 attr = screen->vga_attr;
                    _draw_char_vga(' ', col, row, attr);
                } else if (console_state.mode == CONSOLE_VESA) {
                    _draw_char_vesa(' ', col, row, screen->fg_rgb, screen->bg_rgb);
                }
            }
        }

        return;
    }

    if (ch < 0x20)
        return;

    if (screen->cursor_x >= console_state.cols)
        _newline(screen, screen_index);

    size_t col = screen->cursor_x;
    size_t row = screen->cursor_y;

    console_cell_t* cells = console_screen_cells(screen_index);

    if (cells) {
        console_cell_t* cell = &cells[row * console_state.cols + col];
        cell->codepoint = ch;
        cell->fg = screen->fg_vga;
        cell->bg = screen->bg_vga;
    }

    if (screen_index == console_state.active_screen) {
        if (console_state.mode == CONSOLE_VGA) {
            u8 attr = screen->vga_attr;
            _draw_char_vga(ch, col, row, attr);
        } else if (console_state.mode == CONSOLE_VESA) {
            _draw_char_vesa(ch, col, row, screen->fg_rgb, screen->bg_rgb);
        }
    }

    screen->cursor_x++;
}

static void _put_utf8_byte(console_screen_t* screen, size_t screen_index, u8 byte) {
    if (!screen)
        return;

    if (!screen->utf8_pending_len && byte < 0x80) {
        console_putc(screen, screen_index, byte);
        return;
    }

    if (screen->utf8_pending_len >= sizeof(screen->utf8_pending)) {
        screen->utf8_pending_len = 0;
        console_putc(screen, screen_index, '?');
    }

    screen->utf8_pending[screen->utf8_pending_len++] = byte;

    size_t needed = utf8_sequence_len(screen->utf8_pending[0]);

    if (!needed) {
        screen->utf8_pending_len = 0;
        console_putc(screen, screen_index, '?');
        return;
    }

    if (screen->utf8_pending_len < needed) {
        if (screen->utf8_pending_len > 1 && (byte & 0xc0) != 0x80) {
            screen->utf8_pending_len = 0;
            console_putc(screen, screen_index, '?');

            if (byte < 0x80)
                console_putc(screen, screen_index, byte);
            else if (utf8_sequence_len(byte) > 1)
                screen->utf8_pending[screen->utf8_pending_len++] = byte;
            else
                console_putc(screen, screen_index, '?');
        }
        return;
    }

    u32 codepoint = 0;
    size_t decoded = utf8_decode(screen->utf8_pending, needed, &codepoint);

    screen->utf8_pending_len = 0;

    if (!decoded) {
        console_putc(screen, screen_index, '?');
        return;
    }

    console_putc(screen, screen_index, codepoint);
}

static void _write_screen(size_t screen_index, const char* buf, size_t len) {
    if (!buf || !len || !console_state.ready || console_state.mode == CONSOLE_DISABLED)
        return;

    console_screen_t* screen = _get_screen(screen_index);
    if (!screen)
        return;

    bool batch_cursor = (screen_index == console_state.active_screen);
    bool batch_flush = batch_cursor && console_has_back_buffer();

    if (batch_cursor) {
        _cursor_hide();
        console_state.cursor_batch = true;
    }

    if (batch_flush)
        console_state.flush_batch = true;

    bool esc = false;
    bool csi = false;
    int params[8] = {0};
    size_t param_count = 0;
    int current = -1;

    for (size_t i = 0; i < len; i++) {
        u8 ch = (u8)buf[i];

        if (!esc) {
            if (ch == '\x1b') {
                if (screen->utf8_pending_len) {
                    screen->utf8_pending_len = 0;
                    console_putc(screen, screen_index, '?');
                }
                esc = true;
                csi = false;
                continue;
            }

            if (ch < 0x20 || ch == 0x7f) {
                if (screen->utf8_pending_len) {
                    screen->utf8_pending_len = 0;
                    console_putc(screen, screen_index, '?');
                }
                console_putc(screen, screen_index, ch);
                continue;
            }

            console_put_utf8_byte(screen, screen_index, ch);
            continue;
        }

        if (!csi) {
            if (ch == '[') {
                csi = true;
                param_count = 0;
                current = -1;
                continue;
            }

            esc = false;
            continue;
        }

        if (ch >= '0' && ch <= '9') {
            if (current < 0)
                current = 0;

            current = current * 10 + (ch - '0');
            continue;
        }

        if (ch == ';') {
            if (param_count < (sizeof(params) / sizeof(params[0])))
                params[param_count++] = (current < 0) ? 0 : current;

            current = -1;

            continue;
        }

        if (ch == 'm') {
            if (param_count < (sizeof(params) / sizeof(params[0]))) {
                if (current < 0 && !param_count) {
                    params[param_count++] = 0;
                } else if (current >= 0) {
                    params[param_count++] = current;
                }
            }

            for (size_t p = 0; p < param_count; p++)
                _apply_sgr(screen, params[p]);

            esc = false;
            csi = false;

            continue;
        }

        if (ch >= '@' && ch <= '~') {
            if (current >= 0 && param_count < (sizeof(params) / sizeof(params[0]))) {
                params[param_count++] = current;
                current = -1;
            }

            switch (ch) {
            case 'J': {
                int mode = 0;

                if (!param_count)
                    mode = 0;
                else
                    mode = params[0];

                _handle_csi_clear(screen_index, screen, mode);
                break;
            }
            case 'K': {
                int mode = 0;

                if (!param_count)
                    mode = 0;
                else
                    mode = params[0];

                _handle_csi_clear_line(screen_index, screen, mode);
                break;
            }
            case 'A': {
                int count = 1;

                if (param_count >= 1 && params[0] > 0)
                    count = params[0];

                _handle_csi_move(screen_index, screen, -count, 0);
                break;
            }
            case 'B': {
                int count = 1;
                if (param_count >= 1 && params[0] > 0)
                    count = params[0];

                _handle_csi_move(screen_index, screen, count, 0);
                break;
            }
            case 'C': {
                int count = 1;

                if (param_count >= 1 && params[0] > 0)
                    count = params[0];

                _handle_csi_move(screen_index, screen, 0, count);
                break;
            }
            case 'D': {
                int count = 1;

                if (param_count >= 1 && params[0] > 0)
                    count = params[0];

                _handle_csi_move(screen_index, screen, 0, -count);
                break;
            }
            case 'G': {
                int col = 1;

                if (param_count >= 1)
                    col = params[0];

                _handle_csi_cursor(screen_index, screen, (int)screen->cursor_y + 1, col);
                break;
            }
            case 'H':
            case 'f': {
                int row = 1;
                int col = 1;

                if (param_count >= 1)
                    row = params[0];

                if (param_count >= 2)
                    col = params[1];

                _handle_csi_cursor(screen_index, screen, row, col);
                break;
            }
            case 's':
                screen->saved_cursor_x = screen->cursor_x;
                screen->saved_cursor_y = screen->cursor_y;
                screen->saved_cursor_valid = true;
                break;
            case 'u':
                if (screen->saved_cursor_valid) {
                    screen->cursor_x = screen->saved_cursor_x;
                    screen->cursor_y = screen->saved_cursor_y;
                    _cursor_show(screen_index);
                }
                break;
            default:
                break;
            }

            esc = false;
            csi = false;
        }
    }

    if (batch_cursor) {
        if (batch_flush) {
            console_state.flush_batch = false;
            _flush_dirty();
        }
        console_state.cursor_batch = false;
        _cursor_show(screen_index);
    }
}

static void _free_screens(void) {
    if (console_state.screens && console_state.screens != &console_state.fallback_screen)
        free(console_state.screens);

    if (console_state.cells)
        free(console_state.cells);

    console_state.screens = NULL;
    console_state.cells = NULL;
}

static void _init_screens(size_t active_screen) {
    console_state.screen_count = TTY_SCREEN_COUNT;
    if (!console_state.screen_count)
        console_state.screen_count = 1;

    if (active_screen >= console_state.screen_count)
        active_screen = TTY_CONSOLE;

    size_t count = console_cell_count();

    console_state.screens = calloc(console_state.screen_count, sizeof(console_screen_t));
    console_state.cells = calloc(console_state.screen_count * count, sizeof(console_cell_t));

    if (!console_state.screens || !console_state.cells) {
        if (console_state.screens)
            free(console_state.screens);

        if (console_state.cells)
            free(console_state.cells);

        console_state.screen_count = 1;
        console_state.screens = &console_state.fallback_screen;
        console_state.cells = NULL;
        console_state.active_screen = 0;

        console_screen_reset(&console_state.fallback_screen);

        return;
    }

    console_state.active_screen = active_screen;

    for (size_t i = 0; i < console_state.screen_count; i++) {
        _screen_reset(&console_state.screens[i]);
        _clear_screen_buffer(i);
    }
}

void console_set_font(const font_t* font) {
    if (!font)
        return;

    _use_font(font);

    if (!console_state.ready || console_state.mode != CONSOLE_VESA)
        return;

    console_state.cols = console_state.width / console_state.font_width;
    console_state.rows = console_state.height / console_state.font_height;

    if (!console_state.cols || !console_state.rows) {
        _use_font(&default_font);

        console_state.cols = console_state.width / console_state.font_width;
        console_state.rows = console_state.height / console_state.font_height;
    }

    size_t active = console_state.active_screen;

    console_free_screens();
    console_init_screens(active);
    _redraw_screen(console_state.active_screen);
}

void console_init(const boot_info_t* info) {
    if (!info)
        return;

    memset(&console_state, 0, sizeof(console_state));
    _use_font(&default_font);

    if (info->video.mode == VIDEO_GRAPHICS && info->video.framebuffer && info->video.width &&
        info->video.height && info->video.bytes_per_pixel) {
        u32 pitch = info->video.bytes_per_line;
        if (!pitch)
            pitch = info->video.width * info->video.bytes_per_pixel;

        size_t size = (size_t)pitch * (size_t)info->video.height;

#if defined(__i386__)
        if (size <= PHYS_WINDOW_SIZE_32) {
            console_state.fb_phys = info->video.framebuffer;
            console_state.fb_size = size;

            console_state.width = info->video.width;
            console_state.height = info->video.height;
            console_state.pitch = pitch;
            console_state.bytes_per_pixel = (u8)info->video.bytes_per_pixel;

            console_state.cols = console_state.width / console_state.font_width;
            console_state.rows = console_state.height / console_state.font_height;

            if (!console_state.cols || !console_state.rows) {
                memset(&console_state, 0, sizeof(console_state));
                goto fallback_vga;
            }

            console_state.mode = CONSOLE_VESA;
            console_state.use_phys_window = true;
            console_state.ready = true;
            console_state.fb_back = calloc(1, size);

            console_init_screens(TTY_CONSOLE);
            _redraw_screen(console_state.active_screen);

            return;
        }
#else
        {
            console_state.fb = arch_phys_map(info->video.framebuffer, size);
            console_state.fb_phys = info->video.framebuffer;
            console_state.fb_size = size;

            console_state.width = info->video.width;
            console_state.height = info->video.height;
            console_state.pitch = pitch;
            console_state.bytes_per_pixel = (u8)info->video.bytes_per_pixel;

            console_state.cols = console_state.width / console_state.font_width;
            console_state.rows = console_state.height / console_state.font_height;

            if (!console_state.cols || !console_state.rows) {
                arch_phys_unmap(console_state.fb, size);
                memset(&console_state, 0, sizeof(console_state));
                goto fallback_vga;
            }

            console_state.mode = CONSOLE_VESA;
            console_state.ready = true;
            console_state.fb_back = calloc(1, size);

            console_init_screens(TTY_CONSOLE);
            _redraw_screen(console_state.active_screen);

            return;
        }
#endif
    }

fallback_vga:
    if (info->video.mode == VIDEO_NONE)
        return;

#if defined(__i386__)
    console_state.fb = (u8*)(uintptr_t)VGA_ADDR;
#else
    console_state.fb = arch_phys_map(VGA_ADDR, VGA_WIDTH * VGA_HEIGHT * sizeof(u16));
#endif
    console_state.fb_phys = VGA_ADDR;
    console_state.fb_size = VGA_WIDTH * VGA_HEIGHT * sizeof(u16);

    console_state.width = VGA_WIDTH;
    console_state.height = VGA_HEIGHT;
    console_state.pitch = VGA_WIDTH * sizeof(u16);
    console_state.bytes_per_pixel = 2;

    console_state.cols = VGA_WIDTH;
    console_state.rows = VGA_HEIGHT;

    console_state.mode = CONSOLE_VGA;
    console_state.ready = true;

    _init_screens(TTY_CONSOLE);
    _redraw_screen(console_state.active_screen);
}

ssize_t arch_console_read(void* buf, size_t len) {
    if (!buf)
        return -1;

    u8* out = buf;

    for (size_t i = 0; i < len; i++)
        out[i] = (u8)receive_serial(SERIAL_COM1);

    return (ssize_t)len;
}

ssize_t arch_console_write_screen(size_t screen, const void* buf, size_t len) {
    if (!buf)
        return -1;

    if (!len)
        return 0;

    unsigned long flags = arch_irq_save();

    bool mirror = (screen == TTY_CONSOLE);

    if (!mirror && console_state.ready && screen == console_state.active_screen)
        mirror = true;

    if (!mirror && console_state.ready && screen >= console_state.screen_count)
        mirror = true;

    if (mirror)
        send_serial_sized_string(SERIAL_COM1, buf, len);

    console_write_screen(screen, buf, len);

    arch_irq_restore(flags);

    return (ssize_t)len;
}

ssize_t arch_console_write(const void* buf, size_t len) {
    return arch_console_write_screen(TTY_CONSOLE, buf, len);
}

bool arch_console_set_active(size_t screen) {
    return _set_active(screen);
}

bool arch_console_get_size(size_t* cols, size_t* rows) {
    if (!cols || !rows)
        return false;

    if (!console_state.ready || console_state.mode == CONSOLE_DISABLED)
        return false;

    *cols = console_state.cols;
    *rows = console_state.rows;

    return true;
}

ssize_t arch_tty_read(void* buf, size_t len) {
    return arch_console_read(buf, len);
}

ssize_t arch_tty_write(const void* buf, size_t len) {
    return arch_console_write_screen(console_state.active_screen, buf, len);
}
