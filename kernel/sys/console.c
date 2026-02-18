#include "console.h"

#include <arch/arch.h>
#include <base/macros.h>
#include <base/types.h>
#include <base/utf8.h>
#include <errno.h>
#include <gui/pixel.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/font.h>
#include <sys/tty.h>
#include <term/ansi.h>

typedef struct {
    size_t cursor_x;
    size_t cursor_y;
    size_t saved_cursor_x;
    size_t saved_cursor_y;
    bool saved_cursor_valid;
    u8 fg_idx;
    u8 bg_idx;
    u32 fg_rgb;
    u32 bg_rgb;
    bool bright;
    u8 utf8_pending[4];
    size_t utf8_pending_len;
    ansi_parser_t ansi;
} console_screen_t;

typedef struct {
    u32 codepoint;
    u8 fg;
    u8 bg;
} console_cell_t;

typedef struct {
    console_mode_t mode;
    u8* fb;
    size_t fb_size;
    u8* fb_back;
    u32 width;
    u32 height;
    u32 pitch;
    u8 bytes_per_pixel;
    u8 red_shift;
    u8 green_shift;
    u8 blue_shift;
    u8 red_size;
    u8 green_size;
    u8 blue_size;
    size_t cols;
    size_t rows;
    const font_t* font;
    u32 font_width;
    u32 font_height;
    u32 font_row_bytes;
    u32 font_glyph_bytes;
    font_map_t* font_map_sorted;
    u32 font_map_sorted_count;
    const font_t* font_map_sorted_src;
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
    bool fb_owned;
    pid_t fb_owner;
    size_t fb_owner_screen;
} console_state_t;

static const console_backend_ops_t* backend_ops = NULL;
static console_state_t console_state = {0};

static const u32 ansi_rgb[16] = {
    0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xc0c0c0, 0x808080, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff,
};

#define CONSOLE_TAB_WIDTH 4

void console_backend_register(const console_backend_ops_t* ops) {
    backend_ops = ops;
}

static void _screen_reset_colors(console_screen_t* screen) {
    if (!screen)
        return;

    screen->bright = false;
    screen->fg_idx = 0x7;
    screen->bg_idx = 0x0;
    screen->fg_rgb = ansi_rgb[screen->fg_idx];
    screen->bg_rgb = ansi_rgb[screen->bg_idx];
}

static void _screen_reset(console_screen_t* screen) {
    if (!screen)
        return;

    _screen_reset_colors(screen);

    screen->cursor_x = 0;
    screen->cursor_y = 0;
    screen->saved_cursor_x = 0;
    screen->saved_cursor_y = 0;
    screen->saved_cursor_valid = false;
    screen->utf8_pending_len = 0;
    ansi_parser_reset(&screen->ansi);
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
        cells[i].fg = screen->fg_idx;
        cells[i].bg = screen->bg_idx;
    }
}

static void _update_text_cursor(size_t col, size_t row) {
    if (!console_state.cols || !console_state.rows)
        return;

    if (col >= console_state.cols)
        col = console_state.cols - 1;
    if (row >= console_state.rows)
        row = console_state.rows - 1;

    if (backend_ops && backend_ops->text_cursor_set)
        backend_ops->text_cursor_set(col, row);
}

static void _free_font_map_index(void) {
    if (!console_state.font_map_sorted)
        return;

    free(console_state.font_map_sorted);
    console_state.font_map_sorted = NULL;
    console_state.font_map_sorted_count = 0;
    console_state.font_map_sorted_src = NULL;
}

static void _build_font_map_index(const font_t* font) {
    if (!font || !font->map || !font->map_count) {
        _free_font_map_index();
        return;
    }

    if (console_state.font_map_sorted_src == font && console_state.font_map_sorted &&
        console_state.font_map_sorted_count == font->map_count)
        return;

    _free_font_map_index();

    font_map_t* sorted = malloc(sizeof(font_map_t) * font->map_count);
    if (!sorted)
        return;

    memcpy(sorted, font->map, sizeof(font_map_t) * font->map_count);

    for (u32 i = 1; i < font->map_count; i++) {
        font_map_t current = sorted[i];
        u32 j = i;

        while (j > 0 && sorted[j - 1].codepoint > current.codepoint) {
            sorted[j] = sorted[j - 1];
            j--;
        }

        sorted[j] = current;
    }

    console_state.font_map_sorted = sorted;
    console_state.font_map_sorted_count = font->map_count;
    console_state.font_map_sorted_src = font;
}

static void _use_font(const font_t* font) {
    if (!font || !font->glyphs || !font->glyph_width || !font->glyph_height)
        return;

    console_state.font = font;
    console_state.font_width = font->glyph_width;
    console_state.font_height = font->glyph_height;
    console_state.font_row_bytes = font_row_bytes(font);
    console_state.font_glyph_bytes = font_glyph_bytes(font);
    _build_font_map_index(font);
}

static u32 _font_index(u32 codepoint) {
    const font_t* font = console_state.font;
    if (!font)
        return 0;

    if (console_state.font_map_sorted && console_state.font_map_sorted_count) {
        u32 lo = 0;
        u32 hi = console_state.font_map_sorted_count;

        while (lo < hi) {
            u32 mid = lo + (hi - lo) / 2;
            const font_map_t* entry = &console_state.font_map_sorted[mid];

            if (entry->codepoint == codepoint)
                return entry->glyph;

            if (entry->codepoint < codepoint)
                lo = mid + 1;
            else
                hi = mid;
        }
    } else if (font->map && font->map_count) {
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

    if (backend_ops && backend_ops->fb_map)
        return backend_ops->fb_map(offset, size);

    if (!console_state.fb)
        return NULL;

    return console_state.fb + offset;
}

static void _unmap_range(void* ptr, size_t size) {
    if (backend_ops && backend_ops->fb_unmap)
        backend_ops->fb_unmap(ptr, size);

    (void)ptr;
    (void)size;
}

static bool _has_back_buffer(void) {
    return console_state.mode == CONSOLE_FRAMEBUFFER && console_state.fb_back && console_state.fb_size > 0;
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

    // WM ownership only blocks flushes while its owner VT is active.
    if (console_state.fb_owned && console_state.active_screen == console_state.fb_owner_screen)
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

    _unmap_range(fb, map_size);
    console_state.dirty = false;
}

static void _maybe_flush_dirty(void) {
    if (console_state.flush_batch)
        return;

    _flush_dirty();
}

static void _write_pixel(u8* dst, u32 color) {
    u32 packed = pixel_pack_rgb888(
        color,
        console_state.red_shift,
        console_state.green_shift,
        console_state.blue_shift,
        console_state.red_size,
        console_state.green_size,
        console_state.blue_size
    );
    pixel_store_packed(dst, console_state.bytes_per_pixel, packed);
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

        _mark_dirty_rect(x, y, width, height);
        _maybe_flush_dirty();

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

    _unmap_range(base, map_size);
}

static void _clear_text(const console_screen_t* screen) {
    if (!console_state.fb || !screen || !backend_ops || !backend_ops->text_clear)
        return;

    backend_ops->text_clear(
        console_state.fb, console_state.cols, console_state.rows, screen->fg_idx, screen->bg_idx
    );
}

static void _clear_fb(const console_screen_t* screen) {
    if (!screen)
        return;

    _fill_rect(0, 0, console_state.width, console_state.height, screen->bg_rgb);
}

static void _scroll_text(const console_screen_t* screen) {
    if (!console_state.fb || !screen || !backend_ops || !backend_ops->text_scroll_up)
        return;

    backend_ops->text_scroll_up(
        console_state.fb, console_state.cols, console_state.rows, screen->fg_idx, screen->bg_idx
    );
}

static void _scroll_fb(const console_screen_t* screen) {
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
        _unmap_range(fb, console_state.fb_size);
    }

    _fill_rect(0, console_state.height - console_state.font_height, console_state.width, console_state.font_height, screen->bg_rgb);

    _maybe_flush_dirty();
}

static void _draw_char_fb(u32 codepoint, size_t col, size_t row, u32 fg_rgb, u32 bg_rgb) {
    if (!console_state.fb_size || !console_state.font || !console_state.font_width || !console_state.font_height)
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

            _write_pixel(row_base + gx * console_state.bytes_per_pixel, color);
        }
    }

    if (_has_back_buffer()) {
        _mark_dirty_rect(x, y, console_state.font_width, console_state.font_height);
        _maybe_flush_dirty();
    } else {
        _unmap_range(base, map_size);
    }
}

static void _draw_char_text(u32 codepoint, size_t col, size_t row, u8 fg, u8 bg) {
    if (!console_state.fb || !backend_ops || !backend_ops->text_put)
        return;

    backend_ops->text_put(console_state.fb, console_state.cols, col, row, codepoint, fg, bg);
}


static void _cursor_hide(void) {
    if (!console_state.cursor_drawn)
        return;

    if (console_state.mode == CONSOLE_TEXT) {
        console_state.cursor_drawn = false;
        return;
    }

    if (console_state.mode != CONSOLE_FRAMEBUFFER)
        return;

    console_screen_t* screen = _get_screen(console_state.active_screen);
    console_cell_t* cells = _screen_cells(console_state.active_screen);

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

    _draw_char_fb(codepoint, col, row, fg, bg);
    console_state.cursor_drawn = false;
}

static void _cursor_show(size_t screen_index) {
    if (console_state.cursor_batch)
        return;

    if (screen_index != console_state.active_screen)
        return;

    console_screen_t* screen = _get_screen(screen_index);
    console_cell_t* cells = _screen_cells(screen_index);

    if (!screen || !cells || !console_state.cols || !console_state.rows)
        return;

    if (console_state.mode == CONSOLE_TEXT) {
        _update_text_cursor(screen->cursor_x, screen->cursor_y);
        console_state.cursor_drawn = true;
        console_state.cursor_draw_x = screen->cursor_x;
        console_state.cursor_draw_y = screen->cursor_y;
        return;
    }

    if (console_state.mode != CONSOLE_FRAMEBUFFER)
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

    _draw_char_fb(codepoint, col, row, fg, bg);

    console_state.cursor_drawn = true;
    console_state.cursor_draw_x = col;
    console_state.cursor_draw_y = row;
}

static void _clear_screen_range(size_t index, size_t start, size_t end) {
    console_screen_t* screen = _get_screen(index);
    console_cell_t* cells = _screen_cells(index);

    if (!screen)
        return;

    size_t count = _cell_count();
    if (!count || start >= count)
        return;

    if (end > count)
        end = count;

    bool full_clear = !start && end == count;

    if (cells) {
        for (size_t i = start; i < end; i++) {
            cells[i].codepoint = ' ';
            cells[i].fg = screen->fg_idx;
            cells[i].bg = screen->bg_idx;
        }
    }

    if (index != console_state.active_screen)
        return;

    bool temp_batch = false;

    if (_has_back_buffer() && !console_state.flush_batch) {
        console_state.flush_batch = true;
        temp_batch = true;
    }

    _cursor_hide();

    if (full_clear) {
        if (console_state.mode == CONSOLE_TEXT)
            _clear_text(screen);
        else if (console_state.mode == CONSOLE_FRAMEBUFFER)
            _clear_fb(screen);

        console_state.cursor_drawn = false;

        if (temp_batch) {
            console_state.flush_batch = false;
            _flush_dirty();
        }
        return;
    }

    if (console_state.mode == CONSOLE_TEXT) {
        for (size_t i = start; i < end; i++) {
            size_t row = i / console_state.cols;
            size_t col = i % console_state.cols;
            _draw_char_text(' ', col, row, screen->fg_idx, screen->bg_idx);
        }
    } else if (console_state.mode == CONSOLE_FRAMEBUFFER) {
        for (size_t i = start; i < end; i++) {
            size_t row = i / console_state.cols;
            size_t col = i % console_state.cols;
            _draw_char_fb(' ', col, row, screen->fg_rgb, screen->bg_rgb);
        }
    }

    console_state.cursor_drawn = false;

    if (temp_batch) {
        console_state.flush_batch = false;
        _flush_dirty();
    }
}

static void _handle_csi_clear(size_t index, console_screen_t* screen, int mode) {
    size_t cols = console_state.cols;
    size_t rows = console_state.rows;
    size_t count = _cell_count();

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

static void _handle_csi_move(size_t index, console_screen_t* screen, int row_delta, int col_delta) {
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
    console_screen_t* screen = _get_screen(index);
    console_cell_t* cells = _screen_cells(index);

    if (!screen || !console_state.ready)
        return;

    bool temp_batch = false;

    if (index == console_state.active_screen && _has_back_buffer() && !console_state.flush_batch) {
        console_state.flush_batch = true;
        temp_batch = true;
    }

    if (console_state.mode == CONSOLE_TEXT)
        _clear_text(screen);
    else if (console_state.mode == CONSOLE_FRAMEBUFFER)
        _clear_fb(screen);

    if (!cells) {
        if (temp_batch) {
            console_state.flush_batch = false;
            _flush_dirty();
        }
        return;
    }

    size_t cols = console_state.cols;
    size_t rows = console_state.rows;

    for (size_t row = 0; row < rows; row++) {
        for (size_t col = 0; col < cols; col++) {
            console_cell_t* cell = &cells[row * cols + col];
            u32 codepoint = cell->codepoint ? cell->codepoint : ' ';

            if (console_state.mode == CONSOLE_TEXT) {
                _draw_char_text(codepoint, col, row, cell->fg, cell->bg);
            } else if (console_state.mode == CONSOLE_FRAMEBUFFER) {
                u32 fg = ansi_rgb[cell->fg & 0x0f];
                u32 bg = ansi_rgb[cell->bg & 0x0f];
                _draw_char_fb(codepoint, col, row, fg, bg);
            }
        }
    }

    if (temp_batch) {
        console_state.flush_batch = false;
        _flush_dirty();
    }

    _cursor_show(index);
}

bool console_set_active(size_t index) {
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

    screen->fg_idx = idx;
    screen->fg_rgb = ansi_rgb[idx];
}

static void _set_bg(console_screen_t* screen, u8 base, bool bright) {
    if (!screen)
        return;

    u8 idx = base & 0x7;
    if (bright)
        idx = (u8)(idx + 8);

    screen->bg_idx = idx;
    screen->bg_rgb = ansi_rgb[idx];
}

static void _apply_sgr(console_screen_t* screen, int code) {
    if (!screen)
        return;

    if (!code) {
        _screen_reset_colors(screen);
        return;
    }

    if (code == 1) {
        screen->bright = true;
        _set_fg(screen, screen->fg_idx & 0x7, true);
        return;
    }

    if (code == 2 || code == 22) {
        screen->bright = false;
        _set_fg(screen, screen->fg_idx & 0x7, false);
        return;
    }

    if (code == 39) {
        _set_fg(screen, 0x7, false);
        return;
    }

    if (code == 49) {
        _set_bg(screen, 0x0, false);
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
            last[i].fg = screen->fg_idx;
            last[i].bg = screen->bg_idx;
        }
    }

    if (screen_index != console_state.active_screen)
        return;

    if (console_state.mode == CONSOLE_TEXT)
        _scroll_text(screen);
    else if (console_state.mode == CONSOLE_FRAMEBUFFER)
        _scroll_fb(screen);
}

static void _newline(console_screen_t* screen, size_t screen_index) {
    screen->cursor_x = 0;
    screen->cursor_y++;

    if (screen->cursor_y < console_state.rows)
        return;

    screen->cursor_y = console_state.rows - 1;
    _scroll_screen(screen, screen_index);
}

static size_t _next_tab_stop(size_t cursor_x) {
    size_t next = ((cursor_x / CONSOLE_TAB_WIDTH) + 1) * CONSOLE_TAB_WIDTH;
    return next < console_state.cols ? next : console_state.cols;
}

static void _putc(console_screen_t* screen, size_t screen_index, u32 ch) {
    if (!screen || !console_state.ready || console_state.mode == CONSOLE_DISABLED)
        return;

    if (screen_index == console_state.active_screen)
        _cursor_hide();

    if (ch == '\n') {
        _newline(screen, screen_index);
        _cursor_show(screen_index);
        return;
    }

    if (ch == '\r') {
        screen->cursor_x = 0;
        _cursor_show(screen_index);
        return;
    }

    if (ch == '\t') {
        size_t next = _next_tab_stop(screen->cursor_x);

        while (screen->cursor_x < next)
            _putc(screen, screen_index, ' ');

        _cursor_show(screen_index);
        return;
    }

    if (ch == '\b') {
        if (screen->cursor_x > 0) {
            screen->cursor_x--;

            size_t col = screen->cursor_x;
            size_t row = screen->cursor_y;

            console_cell_t* cells = _screen_cells(screen_index);

            if (cells) {
                console_cell_t* cell = &cells[row * console_state.cols + col];
                cell->codepoint = ' ';
                cell->fg = screen->fg_idx;
                cell->bg = screen->bg_idx;
            }

            if (screen_index == console_state.active_screen) {
                if (console_state.mode == CONSOLE_TEXT) {
                    _draw_char_text(' ', col, row, screen->fg_idx, screen->bg_idx);
                } else if (console_state.mode == CONSOLE_FRAMEBUFFER) {
                    _draw_char_fb(' ', col, row, screen->fg_rgb, screen->bg_rgb);
                }
            }
        }

        _cursor_show(screen_index);
        return;
    }

    if (ch < 0x20) {
        _cursor_show(screen_index);
        return;
    }

    if (screen->cursor_x >= console_state.cols)
        _newline(screen, screen_index);

    size_t col = screen->cursor_x;
    size_t row = screen->cursor_y;

    console_cell_t* cells = _screen_cells(screen_index);

    if (cells) {
        console_cell_t* cell = &cells[row * console_state.cols + col];
        cell->codepoint = ch;
        cell->fg = screen->fg_idx;
        cell->bg = screen->bg_idx;
    }

    if (screen_index == console_state.active_screen) {
        if (console_state.mode == CONSOLE_TEXT) {
            _draw_char_text(ch, col, row, screen->fg_idx, screen->bg_idx);
        } else if (console_state.mode == CONSOLE_FRAMEBUFFER) {
            _draw_char_fb(ch, col, row, screen->fg_rgb, screen->bg_rgb);
        }
    }

    screen->cursor_x++;

    _cursor_show(screen_index);
}

static void _put_utf8_byte(console_screen_t* screen, size_t screen_index, u8 byte) {
    if (!screen)
        return;

    if (!screen->utf8_pending_len && byte < 0x80) {
        _putc(screen, screen_index, byte);
        return;
    }

    if (screen->utf8_pending_len >= sizeof(screen->utf8_pending)) {
        screen->utf8_pending_len = 0;
        _putc(screen, screen_index, '?');
    }

    screen->utf8_pending[screen->utf8_pending_len++] = byte;

    size_t needed = utf8_sequence_len(screen->utf8_pending[0]);

    if (!needed) {
        screen->utf8_pending_len = 0;
        _putc(screen, screen_index, '?');
        return;
    }

    if (screen->utf8_pending_len < needed) {
        if (screen->utf8_pending_len > 1 && (byte & 0xc0) != 0x80) {
            screen->utf8_pending_len = 0;
            _putc(screen, screen_index, '?');

            if (byte < 0x80)
                _putc(screen, screen_index, byte);
            else if (utf8_sequence_len(byte) > 1)
                screen->utf8_pending[screen->utf8_pending_len++] = byte;
            else
                _putc(screen, screen_index, '?');
        }
        return;
    }

    u32 codepoint = 0;
    size_t decoded = utf8_decode(screen->utf8_pending, needed, &codepoint);

    screen->utf8_pending_len = 0;

    if (!decoded) {
        _putc(screen, screen_index, '?');
        return;
    }

    _putc(screen, screen_index, codepoint);
}

typedef struct {
    size_t screen_index;
    console_screen_t* screen;
} console_ansi_ctx_t;

static void _flush_invalid_utf8(console_ansi_ctx_t* ctx) {
    if (!ctx || !ctx->screen || !ctx->screen->utf8_pending_len)
        return;

    ctx->screen->utf8_pending_len = 0;
    _putc(ctx->screen, ctx->screen_index, '?');
}

static void _ansi_print(void* opaque, u8 ch) {
    console_ansi_ctx_t* ctx = opaque;
    if (!ctx || !ctx->screen)
        return;

    _put_utf8_byte(ctx->screen, ctx->screen_index, ch);
}

static void _ansi_control(void* opaque, u8 ch) {
    console_ansi_ctx_t* ctx = opaque;
    if (!ctx || !ctx->screen)
        return;

    _flush_invalid_utf8(ctx);
    _putc(ctx->screen, ctx->screen_index, ch);
}

static void _ansi_csi(void* opaque, char op, const int* params, size_t count, bool private_mode) {
    console_ansi_ctx_t* ctx = opaque;
    if (!ctx || !ctx->screen)
        return;

    console_screen_t* screen = ctx->screen;
    size_t screen_index = ctx->screen_index;

    int move = 1;
    int row = 1;
    int col = 1;

    switch (op) {
    case 'm':
        if (!count) {
            _apply_sgr(screen, 0);
            return;
        }

        for (size_t i = 0; i < count; i++)
            _apply_sgr(screen, params[i]);
        break;
    case 'J':
        _handle_csi_clear(screen_index, screen, ansi_param(params, count, 0, 0));
        break;
    case 'K':
        _handle_csi_clear_line(screen_index, screen, ansi_param(params, count, 0, 0));
        break;
    case 'A':
        move = ansi_param(params, count, 0, 1);
        if (move < 1)
            move = 1;

        _handle_csi_move(screen_index, screen, -move, 0);
        break;
    case 'B':
        move = ansi_param(params, count, 0, 1);
        if (move < 1)
            move = 1;

        _handle_csi_move(screen_index, screen, move, 0);
        break;
    case 'C':
        move = ansi_param(params, count, 0, 1);
        if (move < 1)
            move = 1;
        _handle_csi_move(screen_index, screen, 0, move);
        break;
    case 'D':
        move = ansi_param(params, count, 0, 1);
        if (move < 1)
            move = 1;

        _handle_csi_move(screen_index, screen, 0, -move);
        break;
    case 'G':
        col = ansi_param(params, count, 0, 1);
        _handle_csi_cursor(screen_index, screen, (int)screen->cursor_y + 1, col);
        break;
    case 'H':
    case 'f':
        row = ansi_param(params, count, 0, 1);
        col = ansi_param(params, count, 1, 1);
        _handle_csi_cursor(screen_index, screen, row, col);
        break;
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
    case 'h':
        if (private_mode && ansi_param(params, count, 0, 0) == 25)
            _cursor_show(screen_index);
        break;
    case 'l':
        if (private_mode && ansi_param(params, count, 0, 0) == 25)
            _cursor_hide();
        break;
    default:
        break;
    }
}

static const ansi_callbacks_t _ansi_callbacks = {
    .on_print = _ansi_print,
    .on_control = _ansi_control,
    .on_csi = _ansi_csi,
    .on_escape = NULL,
};

static void _write_screen_locked(size_t screen_index, const char* buf, size_t len) {
    if (!buf || !len || !console_state.ready || console_state.mode == CONSOLE_DISABLED)
        return;

    console_screen_t* screen = _get_screen(screen_index);
    if (!screen)
        return;

    bool batch_cursor = (screen_index == console_state.active_screen);
    bool batch_flush = batch_cursor && _has_back_buffer();

    if (batch_cursor) {
        _cursor_hide();
        console_state.cursor_batch = true;
    }

    if (batch_flush)
        console_state.flush_batch = true;

    console_ansi_ctx_t ansi_ctx = {
        .screen_index = screen_index,
        .screen = screen,
    };

    for (size_t i = 0; i < len; i++) {
        u8 ch = (u8)buf[i];

        if (ch == '\x1b')
            _flush_invalid_utf8(&ansi_ctx);

        ansi_parser_feed(&screen->ansi, ch, &_ansi_callbacks, &ansi_ctx);
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

    size_t count = _cell_count();

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

        _screen_reset(&console_state.fallback_screen);

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

    size_t old_cols = console_state.cols;
    size_t old_rows = console_state.rows;
    bool preserve = false;

    _use_font(font);

    if (!console_state.ready || console_state.mode != CONSOLE_FRAMEBUFFER)
        return;

    console_state.cols = console_state.width / console_state.font_width;
    console_state.rows = console_state.height / console_state.font_height;

    if (!console_state.cols || !console_state.rows) {
        _use_font(&default_font);

        console_state.cols = console_state.width / console_state.font_width;
        console_state.rows = console_state.height / console_state.font_height;
    }

    preserve = console_state.screens && console_state.cells && console_state.cols == old_cols &&
               console_state.rows == old_rows;

    if (preserve) {
        _redraw_screen(console_state.active_screen);
        return;
    }

    size_t active = console_state.active_screen;

    _free_screens();
    _init_screens(active);
    _redraw_screen(console_state.active_screen);
}

void console_init(void* arch_boot_info) {
    memset(&console_state, 0, sizeof(console_state));
    _use_font(&default_font);

    if (!backend_ops || !backend_ops->probe)
        return;

    console_hw_desc_t hw = {0};
    if (!backend_ops->probe(arch_boot_info, &hw))
        return;

    if (hw.mode == CONSOLE_DISABLED)
        return;

    console_state.mode = hw.mode;
    console_state.fb = hw.fb;
    console_state.fb_size = hw.fb_size;
    console_state.width = hw.width;
    console_state.height = hw.height;
    console_state.pitch = hw.pitch;
    console_state.bytes_per_pixel = hw.bytes_per_pixel;
    console_state.red_shift = hw.red_shift;
    console_state.green_shift = hw.green_shift;
    console_state.blue_shift = hw.blue_shift;
    console_state.red_size = hw.red_size;
    console_state.green_size = hw.green_size;
    console_state.blue_size = hw.blue_size;

    if (console_state.mode == CONSOLE_FRAMEBUFFER) {
        pixel_apply_legacy_defaults(
            console_state.bytes_per_pixel,
            &console_state.red_shift,
            &console_state.green_shift,
            &console_state.blue_shift,
            &console_state.red_size,
            &console_state.green_size,
            &console_state.blue_size
        );

        console_state.cols = console_state.width / console_state.font_width;
        console_state.rows = console_state.height / console_state.font_height;

        if (!console_state.cols || !console_state.rows) {
            memset(&console_state, 0, sizeof(console_state));
            return;
        }

        console_state.fb_back = calloc(1, console_state.fb_size);
    } else if (console_state.mode == CONSOLE_TEXT) {
        console_state.cols = console_state.width;
        console_state.rows = console_state.height;
    } else {
        memset(&console_state, 0, sizeof(console_state));
        return;
    }

    console_state.ready = true;
    _init_screens(TTY_CONSOLE);
    _redraw_screen(console_state.active_screen);
}

ssize_t console_read(void* buf, size_t len) {
    if (!buf)
        return -EFAULT;

    if (!len)
        return 0;

    return -ENOSYS;
}

ssize_t console_write_screen(size_t screen, const void* buf, size_t len) {
    if (!buf)
        return -EFAULT;

    if (!len)
        return 0;

    unsigned long flags = arch_irq_save();
    _write_screen_locked(screen, buf, len);
    arch_irq_restore(flags);

    return (ssize_t)len;
}

ssize_t console_write(const void* buf, size_t len) {
    return console_write_screen(TTY_CONSOLE, buf, len);
}

bool console_get_size(size_t* cols, size_t* rows) {
    if (!cols || !rows)
        return false;

    if (!console_state.ready || console_state.mode == CONSOLE_DISABLED)
        return false;

    *cols = console_state.cols;
    *rows = console_state.rows;

    return true;
}

int console_fb_acquire(pid_t pid, size_t screen) {
    if (pid <= 0)
        return -EINVAL;

    if (!console_state.ready || console_state.mode != CONSOLE_FRAMEBUFFER || !_has_back_buffer())
        return -ENODEV;

    if (screen >= console_state.screen_count)
        return -EINVAL;

    unsigned long irq_flags = arch_irq_save();

    if (console_state.fb_owned && console_state.fb_owner != pid) {
        arch_irq_restore(irq_flags);
        return -EBUSY;
    }

    console_state.fb_owned = true;
    console_state.fb_owner = pid;
    console_state.fb_owner_screen = screen;

    arch_irq_restore(irq_flags);
    return 0;
}

int console_fb_release(pid_t pid) {
    if (pid <= 0)
        return -EINVAL;

    if (!console_state.ready || console_state.mode != CONSOLE_FRAMEBUFFER || !_has_back_buffer())
        return -ENODEV;

    unsigned long irq_flags = arch_irq_save();

    if (!console_state.fb_owned) {
        arch_irq_restore(irq_flags);
        return 0;
    }

    if (console_state.fb_owner != pid) {
        arch_irq_restore(irq_flags);
        return -EPERM;
    }

    console_state.fb_owned = false;
    console_state.fb_owner = 0;
    console_state.fb_owner_screen = TTY_CONSOLE;
    console_state.dirty = false;

    _redraw_screen(console_state.active_screen);

    arch_irq_restore(irq_flags);
    return 0;
}

ssize_t console_fb_owner_screen(void) {
    if (!console_state.ready || console_state.mode != CONSOLE_FRAMEBUFFER || !_has_back_buffer())
        return TTY_NONE;

    unsigned long irq_flags = arch_irq_save();
    ssize_t owner_screen = console_state.fb_owned ? (ssize_t)console_state.fb_owner_screen : TTY_NONE;
    arch_irq_restore(irq_flags);

    return owner_screen;
}

void console_panic(void) {
    if (!console_state.ready || console_state.mode == CONSOLE_DISABLED)
        return;

    unsigned long irq_flags = arch_irq_save();

    if (console_state.mode == CONSOLE_FRAMEBUFFER && _has_back_buffer()) {
        console_state.fb_owned = false;
        console_state.fb_owner = 0;
        console_state.fb_owner_screen = TTY_CONSOLE;
        console_state.flush_batch = false;
        console_state.dirty = false;
    }

    if (console_state.active_screen != TTY_CONSOLE)
        console_state.active_screen = TTY_CONSOLE;

    _redraw_screen(TTY_CONSOLE);

    arch_irq_restore(irq_flags);
}
