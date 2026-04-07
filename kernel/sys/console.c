#include "console.h"

#include <arch/arch.h>
#include <base/macros.h>
#include <base/types.h>
#include <errno.h>
#include <gui/pixel.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/font.h>
#include <sys/lock.h>
#include <sys/tty.h>
#include <term/ansi.h>
#include <term/cells.h>
#include <term/cursor.h>
#include <term/glyph.h>
#include <term/utf8.h>
#include "ws.h"

typedef struct {
    size_t cursor_x;
    size_t cursor_y;
    size_t saved_cursor_x;
    size_t saved_cursor_y;
    bool saved_cursor_valid;
    ansi_color_state_t color;
    term_utf8_state_t utf8;
    ansi_parser_t ansi;
} console_screen_t;

typedef term_cell_t console_cell_t;

typedef struct {
    console_mode_t mode;
    u8 *fb;
    size_t fb_size;
    u8 *fb_back;
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
    const font_t *font;
    u32 font_width;
    u32 font_height;
    u32 font_cell_width;
    u32 font_cell_height;
    u32 font_cell_src_x;
    u32 font_row_bytes;
    u32 font_glyph_bytes;
    font_map_t *font_map_sorted;
    u32 font_map_sorted_count;
    const font_t *font_map_sorted_src;
    bool ready;
    size_t screen_count;
    size_t active_screen;
    console_screen_t *screens;
    console_cell_t *cells;
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
    bool handoff_refresh_pending;
} console_state_t;

typedef struct {
    size_t screen_index;
    console_screen_t *screen;
} console_ansi_ctx_t;

static const console_backend_ops_t *backend_ops = NULL;
static console_state_t console_state = {0};
static spinlock_t console_lock = SPINLOCK_INIT;

#define CONSOLE_TAB_WIDTH 4

void console_backend_register(const console_backend_ops_t *ops) {
    backend_ops = ops;
}

static void _screen_reset_colors(console_screen_t *screen) {
    if (!screen) {
        return;
    }

    ansi_color_reset(&screen->color);
}

static bool
_glyph_pixel_on(const u8 *glyph, u32 row_bytes, u32 x, u32 y) {
    const u8 *row_ptr = glyph + (size_t)y * row_bytes;
    u8 bits = row_ptr[x / 8];
    u8 mask = (u8)(0x80 >> (x & 7));
    return (bits & mask) != 0;
}

static void _screen_reset(console_screen_t *screen) {
    if (!screen) {
        return;
    }

    _screen_reset_colors(screen);

    screen->cursor_x = 0;
    screen->cursor_y = 0;
    screen->saved_cursor_x = 0;
    screen->saved_cursor_y = 0;
    screen->saved_cursor_valid = false;
    term_utf8_reset(&screen->utf8);

    ansi_parser_reset(&screen->ansi);
}

static size_t _cell_count(void) {
    return console_state.cols * console_state.rows;
}

static console_screen_t *_get_screen(size_t index) {
    if (index >= console_state.screen_count) {
        return NULL;
    }

    if (!console_state.screens) {
        return NULL;
    }

    return &console_state.screens[index];
}

static console_cell_t *_screen_cells(size_t index) {
    if (!console_state.cells) {
        return NULL;
    }

    size_t count = _cell_count();
    return console_state.cells + index * count;
}

static bool _screen_is_blank(size_t index) {
    console_screen_t *screen = _get_screen(index);
    console_cell_t *cells = _screen_cells(index);

    if (!screen) {
        return true;
    }

    if (screen->cursor_x || screen->cursor_y || screen->saved_cursor_valid) {
        return false;
    }

    if (!cells) {
        return true;
    }

    size_t count = _cell_count();
    for (size_t i = 0; i < count; i++) {
        if (cells[i].codepoint != ' ') {
            return false;
        }
    }

    return true;
}

static void _inherit_screen_state(size_t dst, size_t src) {
    console_screen_t *dst_screen = _get_screen(dst);
    console_screen_t *src_screen = _get_screen(src);

    if (!dst_screen || !src_screen || dst == src) {
        return;
    }

    *dst_screen = *src_screen;

    console_cell_t *dst_cells = _screen_cells(dst);
    console_cell_t *src_cells = _screen_cells(src);
    size_t count = _cell_count();

    if (dst_cells && src_cells && count) {
        memcpy(dst_cells, src_cells, count * sizeof(*dst_cells));
    }
}

static void _clear_screen_buffer(size_t index) {
    console_screen_t *screen = _get_screen(index);
    console_cell_t *cells = _screen_cells(index);

    if (!screen || !cells) {
        return;
    }

    size_t cols = console_state.cols;
    size_t rows = console_state.rows;

    if (!cols || !rows) {
        return;
    }

    term_cells_clear(
        cells,
        cols,
        rows,
        screen->color.fg_idx,
        screen->color.bg_idx
    );
}

static void _update_text_cursor(size_t col, size_t row) {
    if (!console_state.cols || !console_state.rows) {
        return;
    }

    if (col >= console_state.cols) {
        col = console_state.cols - 1;
    }
    if (row >= console_state.rows) {
        row = console_state.rows - 1;
    }

    if (backend_ops && backend_ops->text_cursor_set) {
        backend_ops->text_cursor_set(col, row);
    }
}

static void _free_font_map_index(void) {
    if (!console_state.font_map_sorted) {
        return;
    }

    free(console_state.font_map_sorted);
    console_state.font_map_sorted = NULL;
    console_state.font_map_sorted_count = 0;
    console_state.font_map_sorted_src = NULL;
}

static void _build_font_map_index(const font_t *font) {
    if (!font || !font->map || !font->map_count) {
        _free_font_map_index();
        return;
    }

    if (
        console_state.font_map_sorted_src == font &&
        console_state.font_map_sorted &&
        console_state.font_map_sorted_count == font->map_count
    ) {
        return;
    }

    _free_font_map_index();

    font_map_t *sorted = malloc(sizeof(font_map_t) * font->map_count);
    if (!sorted) {
        return;
    }

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

static bool
_font_lookup_mapped(const font_t *font, u32 codepoint, u32 *glyph_out) {
    if (!font || !glyph_out || !font->map || !font->map_count) {
        return false;
    }

    if (
        console_state.font_map_sorted &&
        console_state.font_map_sorted_count &&
        console_state.font_map_sorted_src == font
    ) {
        u32 lo = 0;
        u32 hi = console_state.font_map_sorted_count;

        while (lo < hi) {
            u32 mid = lo + (hi - lo) / 2;
            const font_map_t *entry = &console_state.font_map_sorted[mid];

            if (entry->codepoint == codepoint) {
                if (entry->glyph >= font->glyph_count) {
                    return false;
                }

                *glyph_out = entry->glyph;
                return true;
            }

            if (entry->codepoint < codepoint) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }

        return false;
    }

    for (u32 i = 0; i < font->map_count; i++) {
        if (font->map[i].codepoint != codepoint) {
            continue;
        }

        if (font->map[i].glyph >= font->glyph_count) {
            return false;
        }

        *glyph_out = font->map[i].glyph;
        return true;
    }

    return false;
}

static void _set_default_font_cell_metrics(void) {
    console_state.font_cell_src_x = 0;
    console_state.font_cell_width = console_state.font_width;
    console_state.font_cell_height = console_state.font_height;
}

static bool
_glyph_bounds_for_index(u32 glyph_idx, u32 *left_out, u32 *right_out) {
    if (
        !console_state.font ||
        glyph_idx >= console_state.font->glyph_count ||
        !left_out ||
        !right_out
    ) {
        return false;
    }

    const u8 *glyph =
        console_state.font->glyphs + glyph_idx * console_state.font_glyph_bytes;

    u32 left = console_state.font_width;
    u32 right = console_state.font_height;

    bool any = false;

    for (u32 gy = 0; gy < console_state.font_height; gy++) {
        for (u32 gx = 0; gx < console_state.font_width; gx++) {
            if (!_glyph_pixel_on(glyph, console_state.font_row_bytes, gx, gy)) {
                continue;
            }

            if (gx < left) {
                left = gx;
            }

            if (!any || gx > right) {
                right = gx;
            }

            any = true;
        }
    }

    if (!any) {
        return false;
    }

    *left_out = left;
    *right_out = right;
    return true;
}

static void _derive_font_cell_metrics(void) {
    _set_default_font_cell_metrics();

    if (
        !console_state.font ||
        !console_state.font->glyphs ||
        !console_state.font_width ||
        !console_state.font_height
    ) {
        return;
    }

    u32 min_left = console_state.font_width;
    u32 max_right = 0;
    bool have_bounds = false;

    // Derive terminal cell advance from printable ASCII
    for (u32 cp = 32; cp < 127; cp++) {
        u32 glyph = 0;

        if (console_state.font->map_count) {
            if (!_font_lookup_mapped(console_state.font, cp, &glyph)) {
                continue;
            }
        } else {
            if (cp < console_state.font->first_char) {
                continue;
            }

            glyph = cp - console_state.font->first_char;
            if (glyph >= console_state.font->glyph_count) {
                continue;
            }
        }

        u32 left = 0;
        u32 right = 0;
        if (!_glyph_bounds_for_index(glyph, &left, &right)) {
            continue;
        }

        if (left < min_left) {
            min_left = left;
        }

        if (!have_bounds || right > max_right) {
            max_right = right;
        }

        have_bounds = true;
    }

    if (!have_bounds || max_right < min_left) {
        return;
    }

    u32 advance = max_right - min_left + 1;
    u32 max_advance = console_state.font_width - min_left;

    if (!advance || !max_advance) {
        return;
    }

    if (advance < max_advance) {
        u32 padded = advance + TERM_GLYPH_CELL_GAP_PX;
        if (padded < advance || padded > max_advance) {
            padded = max_advance;
        }
        advance = padded;
    }

    console_state.font_cell_src_x = min_left;
    console_state.font_cell_width = advance;
}

static void _use_font(const font_t *font) {
    if (!font || !font->glyphs || !font->glyph_width || !font->glyph_height) {
        return;
    }

    console_state.font = font;
    console_state.font_width = font->glyph_width;
    console_state.font_height = font->glyph_height;
    console_state.font_row_bytes = font_row_bytes(font);
    console_state.font_glyph_bytes = font_glyph_bytes(font);

    _set_default_font_cell_metrics();
    _build_font_map_index(font);
    _derive_font_cell_metrics();
}

static u32 _font_index(u32 codepoint) {
    const font_t *font = console_state.font;
    if (!font) {
        return 0;
    }

    u32 glyph = 0;

    if (_font_lookup_mapped(font, codepoint, &glyph)) {
        return glyph;
    }

    if (!font->map_count) {
        if (codepoint < font->first_char) {
            return 0;
        }

        u32 idx = codepoint - font->first_char;

        if (idx < font->glyph_count) {
            return idx;
        }
    }

    if (_font_lookup_mapped(font, (u32)'?', &glyph)) {
        return glyph;
    }

    if (!font->map_count && (u32)'?' >= font->first_char) {
        u32 idx = (u32)'?' - font->first_char;
        if (idx < font->glyph_count) {
            return idx;
        }
    }

    return 0;
}

static u8 *_map_range(size_t offset, size_t size) {
    if (!size) {
        return NULL;
    }

    if (backend_ops && backend_ops->fb_map) {
        return backend_ops->fb_map(offset, size);
    }

    if (!console_state.fb) {
        return NULL;
    }

    return console_state.fb + offset;
}

static void _unmap_range(void *ptr, size_t size) {
    if (backend_ops && backend_ops->fb_unmap) {
        backend_ops->fb_unmap(ptr, size);
    }

    (void)ptr;
    (void)size;
}

static bool _has_back_buffer(void) {
    return console_state.mode == CONSOLE_FRAMEBUFFER && console_state.fb_back &&
           console_state.fb_size > 0;
}

static void _mark_dirty_rect(size_t x, size_t y, size_t width, size_t height) {
    if (!_has_back_buffer() || !width || !height) {
        return;
    }

    if (x >= console_state.width || y >= console_state.height) {
        return;
    }

    if (x + width > console_state.width) {
        width = console_state.width - x;
    }

    if (y + height > console_state.height) {
        height = console_state.height - y;
    }

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

    if (x < console_state.dirty_x0) {
        console_state.dirty_x0 = x;
    }

    if (y < console_state.dirty_y0) {
        console_state.dirty_y0 = y;
    }

    if (x1 > console_state.dirty_x1) {
        console_state.dirty_x1 = x1;
    }

    if (y1 > console_state.dirty_y1) {
        console_state.dirty_y1 = y1;
    }
}

static void _flush_dirty(void) {
    if (!_has_back_buffer() || !console_state.dirty) {
        return;
    }

    if (console_state.fb_owned && console_state.active_screen == console_state.fb_owner_screen) {
        return;
    }

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

    u8 *fb = _map_range(offset, map_size);
    if (!fb) {
        return;
    }

    const u8 *src = console_state.fb_back + offset;

    for (size_t row = 0; row < height; row++) {
        memcpy(
            fb + row * console_state.pitch,
            src + row * console_state.pitch,
            width_bytes
        );
    }

    _unmap_range(fb, map_size);
    console_state.dirty = false;
}

static void _maybe_flush_dirty(void) {
    if (console_state.flush_batch) {
        return;
    }

    _flush_dirty();
}

static void _write_pixel(u8 *dst, u32 color) {
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

static void
_fill_rect(size_t x, size_t y, size_t width, size_t height, u32 color) {
    if (!console_state.fb_size || !width || !height) {
        return;
    }

    if (x >= console_state.width || y >= console_state.height) {
        return;
    }

    if (x + width > console_state.width) {
        width = console_state.width - x;
    }

    if (y + height > console_state.height) {
        height = console_state.height - y;
    }

    if (_has_back_buffer()) {
        size_t offset =
            y * console_state.pitch + x * console_state.bytes_per_pixel;

        u8 *base = console_state.fb_back + offset;

        for (size_t row = 0; row < height; row++) {
            u8 *row_base = base + row * console_state.pitch;

            for (size_t col = 0; col < width; col++) {
                _write_pixel(
                    row_base + col * console_state.bytes_per_pixel, color
                );
            }
        }

        _mark_dirty_rect(x, y, width, height);
        _maybe_flush_dirty();

        return;
    }

    size_t width_bytes = width * console_state.bytes_per_pixel;
    size_t map_size = (height - 1) * console_state.pitch + width_bytes;
    size_t offset = y * console_state.pitch + x * console_state.bytes_per_pixel;

    u8 *base = _map_range(offset, map_size);
    if (!base) {
        return;
    }

    for (size_t row = 0; row < height; row++) {
        u8 *row_base = base + row * console_state.pitch;

        for (size_t col = 0; col < width; col++) {
            _write_pixel(row_base + col * console_state.bytes_per_pixel, color);
        }
    }

    _unmap_range(base, map_size);
}

static void _clear_text(const console_screen_t *screen) {
    if (!console_state.fb || !screen || !backend_ops || !backend_ops->text_clear) {
        return;
    }

    backend_ops->text_clear(
        console_state.fb,
        console_state.cols,
        console_state.rows,
        screen->color.fg_idx,
        screen->color.bg_idx
    );
}

static void _clear_fb(const console_screen_t *screen) {
    if (!screen) {
        return;
    }

    _fill_rect(
        0,
        0,
        console_state.width,
        console_state.height,
        ansi_color_rgb(screen->color.bg_idx)
    );
}

static void _scroll_text(const console_screen_t *screen) {
    if (!console_state.fb || !screen || !backend_ops || !backend_ops->text_scroll_up) {
        return;
    }

    backend_ops->text_scroll_up(
        console_state.fb,
        console_state.cols,
        console_state.rows,
        screen->color.fg_idx,
        screen->color.bg_idx
    );
}

static void _scroll_fb(const console_screen_t *screen) {
    if (!screen || !console_state.fb_size || !console_state.font_cell_height) {
        return;
    }

    size_t line_bytes = console_state.pitch * console_state.font_cell_height;
    size_t move_bytes =
        console_state.pitch * (console_state.height - console_state.font_cell_height);

    if (_has_back_buffer()) {
        memmove(
            console_state.fb_back,
            console_state.fb_back + line_bytes,
            move_bytes
        );
        _mark_dirty_rect(0, 0, console_state.width, console_state.height);
    } else {
        u8 *fb = _map_range(0, console_state.fb_size);
        if (!fb) {
            return;
        }

        memmove(fb, fb + line_bytes, move_bytes);
        _unmap_range(fb, console_state.fb_size);
    }

    _fill_rect(
        0,
        console_state.height - console_state.font_cell_height,
        console_state.width,
        console_state.font_cell_height,
        ansi_color_rgb(screen->color.bg_idx)
    );

    _maybe_flush_dirty();
}

static void
_draw_char_fb(u32 codepoint, size_t col, size_t row, u32 fg_rgb, u32 bg_rgb) {
    if (
        !console_state.fb_size ||
        !console_state.font ||
        !console_state.font_width ||
        !console_state.font_height ||
        !console_state.font_cell_width ||
        !console_state.font_cell_height
    ) {
        return;
    }

    size_t x = col * console_state.font_cell_width;
    size_t y = row * console_state.font_cell_height;

    u32 glyph_w = console_state.font_width;
    u32 glyph_h = console_state.font_height;
    u32 glyph_x0 = console_state.font_cell_src_x;

    if (glyph_x0 >= glyph_w) {
        glyph_x0 = 0;
    }

    u32 cell_w = console_state.font_cell_width;
    u32 cell_h = console_state.font_cell_height;
    u32 draw_w = cell_w;
    u32 draw_h = cell_h;
    u32 src_w = glyph_w - glyph_x0;

    if (draw_w > src_w) {
        draw_w = src_w;
    }

    if (draw_h > glyph_h) {
        draw_h = glyph_h;
    }

    if (!draw_w || !draw_h) {
        return;
    }

    size_t width_bytes =
        (size_t)cell_w * console_state.bytes_per_pixel;
    size_t map_size =
        ((size_t)cell_h - 1) * console_state.pitch + width_bytes;

    u32 index = _font_index(codepoint);
    const u8 *glyph =
        console_state.font->glyphs + index * console_state.font_glyph_bytes;

    size_t offset = y * console_state.pitch + x * console_state.bytes_per_pixel;
    u8 *base = NULL;

    if (_has_back_buffer()) {
        base = console_state.fb_back + offset;
    } else {
        base = _map_range(offset, map_size);

        if (!base) {
            return;
        }
    }

    term_pixel_format_t fmt = {
        .bytes_per_pixel = console_state.bytes_per_pixel,
        .red_shift = console_state.red_shift,
        .green_shift = console_state.green_shift,
        .blue_shift = console_state.blue_shift,
        .red_size = console_state.red_size,
        .green_size = console_state.green_size,
        .blue_size = console_state.blue_size,
    };

    u32 bg_packed_full = pixel_pack_rgb888(
        bg_rgb,
        fmt.red_shift,
        fmt.green_shift,
        fmt.blue_shift,
        fmt.red_size,
        fmt.green_size,
        fmt.blue_size
    );

    // Always clear the full terminal cell first so no stale pixels survive
    // around cropped glyph bounds.
    for (u32 by = 0; by < cell_h; by++) {
        u8 *row_base = base + (size_t)by * console_state.pitch;
        for (u32 bx = 0; bx < cell_w; bx++) {
            pixel_store_packed(
                row_base + (size_t)bx * console_state.bytes_per_pixel,
                console_state.bytes_per_pixel,
                bg_packed_full
            );
        }
    }

    if (glyph_x0 == 0 && draw_w == glyph_w && draw_h == glyph_h) {
        term_glyph_blit_packed(
            base,
            console_state.pitch,
            &fmt,
            glyph,
            glyph_w,
            glyph_h,
            console_state.font_row_bytes,
            fg_rgb,
            bg_rgb
        );
    } else {
        u32 fg_packed = pixel_pack_rgb888(
            fg_rgb,
            fmt.red_shift,
            fmt.green_shift,
            fmt.blue_shift,
            fmt.red_size,
            fmt.green_size,
            fmt.blue_size
        );
        for (u32 gy = 0; gy < draw_h; gy++) {
            u8 *row_base = base + (size_t)gy * console_state.pitch;

            for (u32 gx = 0; gx < draw_w; gx++) {
                bool on = _glyph_pixel_on(
                    glyph,
                    console_state.font_row_bytes,
                    glyph_x0 + gx,
                    gy
                );

                pixel_store_packed(
                    row_base + (size_t)gx * console_state.bytes_per_pixel,
                    console_state.bytes_per_pixel,
                    on ? fg_packed : bg_packed_full
                );
            }
        }
    }

    if (_has_back_buffer()) {
        _mark_dirty_rect(x, y, cell_w, cell_h);
        _maybe_flush_dirty();
    } else {
        _unmap_range(base, map_size);
    }
}

static void
_draw_char_text(u32 codepoint, size_t col, size_t row, u8 fg, u8 bg) {
    if (!console_state.fb || !backend_ops || !backend_ops->text_put) {
        return;
    }

    backend_ops->text_put(
        console_state.fb, console_state.cols, col, row, codepoint, fg, bg
    );
}


static void _cursor_hide(void) {
    if (!console_state.cursor_drawn) {
        return;
    }

    if (console_state.mode == CONSOLE_TEXT) {
        console_state.cursor_drawn = false;
        return;
    }

    if (console_state.mode != CONSOLE_FRAMEBUFFER) {
        console_state.cursor_drawn = false;
        return;
    }

    console_screen_t *screen = _get_screen(console_state.active_screen);
    console_cell_t *cells = _screen_cells(console_state.active_screen);

    if (!screen || !cells) {
        console_state.cursor_drawn = false;
        return;
    }

    size_t col = console_state.cursor_draw_x;
    size_t row = console_state.cursor_draw_y;

    if (col >= console_state.cols || row >= console_state.rows) {
        console_state.cursor_drawn = false;
        return;
    }

    console_cell_t *cell = &cells[row * console_state.cols + col];
    u32 codepoint = cell->codepoint ? cell->codepoint : ' ';

    u32 fg = ansi_color_rgb(cell->fg);
    u32 bg = ansi_color_rgb(cell->bg);

    _draw_char_fb(codepoint, col, row, fg, bg);
    console_state.cursor_drawn = false;
}

static void _cursor_show(size_t screen_index) {
    if (console_state.cursor_batch) {
        return;
    }

    if (screen_index != console_state.active_screen) {
        return;
    }

    console_screen_t *screen = _get_screen(screen_index);
    console_cell_t *cells = _screen_cells(screen_index);

    if (!screen || !cells || !console_state.cols || !console_state.rows) {
        return;
    }

    if (console_state.mode == CONSOLE_TEXT) {
        _update_text_cursor(screen->cursor_x, screen->cursor_y);
        console_state.cursor_drawn = true;
        console_state.cursor_draw_x = screen->cursor_x;
        console_state.cursor_draw_y = screen->cursor_y;
        return;
    }

    if (console_state.mode != CONSOLE_FRAMEBUFFER) {
        return;
    }

    _cursor_hide();

    size_t col = screen->cursor_x;
    size_t row = screen->cursor_y;
    if (col >= console_state.cols || row >= console_state.rows) {
        return;
    }

    console_cell_t *cell = &cells[row * console_state.cols + col];
    u32 codepoint = cell->codepoint ? cell->codepoint : ' ';

    u32 fg = ansi_color_rgb(cell->bg);
    u32 bg = ansi_color_rgb(cell->fg);

    _draw_char_fb(codepoint, col, row, fg, bg);

    console_state.cursor_drawn = true;
    console_state.cursor_draw_x = col;
    console_state.cursor_draw_y = row;
}

static void _clear_screen_range(size_t index, size_t start, size_t end) {
    console_screen_t *screen = _get_screen(index);
    console_cell_t *cells = _screen_cells(index);

    if (!screen) {
        return;
    }

    size_t count = _cell_count();
    if (!count || start >= count) {
        return;
    }

    if (end > count) {
        end = count;
    }

    bool full_clear = !start && end == count;

    if (cells) {
        term_cells_clear_range(
            cells,
            count,
            start,
            end,
            screen->color.fg_idx,
            screen->color.bg_idx
        );
    }

    if (index != console_state.active_screen) {
        return;
    }

    bool temp_batch = false;

    if (_has_back_buffer() && !console_state.flush_batch) {
        console_state.flush_batch = true;
        temp_batch = true;
    }

    _cursor_hide();

    if (full_clear) {
        if (console_state.mode == CONSOLE_TEXT) {
            _clear_text(screen);
        } else if (console_state.mode == CONSOLE_FRAMEBUFFER) {
            _clear_fb(screen);
        }

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
            _draw_char_text(
                ' ', col, row, screen->color.fg_idx, screen->color.bg_idx
            );
        }
    } else if (console_state.mode == CONSOLE_FRAMEBUFFER) {
        for (size_t i = start; i < end; i++) {
            size_t row = i / console_state.cols;
            size_t col = i % console_state.cols;
            _draw_char_fb(
                ' ',
                col,
                row,
                ansi_color_rgb(screen->color.fg_idx),
                ansi_color_rgb(screen->color.bg_idx)
            );
        }
    }

    console_state.cursor_drawn = false;

    if (temp_batch) {
        console_state.flush_batch = false;
        _flush_dirty();
    }
}

static void _handle_csi_clear(void *opaque, int mode) {
    console_ansi_ctx_t *ctx = opaque;
    if (!ctx || !ctx->screen) {
        return;
    }

    size_t index = ctx->screen_index;
    console_screen_t *screen = ctx->screen;
    size_t cols = console_state.cols;
    size_t rows = console_state.rows;
    size_t count = _cell_count();

    if (!screen || !cols || !rows || !count) {
        return;
    }

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

static void _handle_csi_clear_line(void *opaque, int mode) {
    console_ansi_ctx_t *ctx = opaque;
    if (!ctx || !ctx->screen) {
        return;
    }

    size_t index = ctx->screen_index;
    console_screen_t *screen = ctx->screen;

    if (!screen || !console_state.cols || !console_state.rows) {
        return;
    }

    size_t row = screen->cursor_y;

    if (row >= console_state.rows) {
        row = console_state.rows - 1;
    }

    size_t row_start = row * console_state.cols;
    size_t row_end = row_start + console_state.cols;
    size_t cursor = row_start + screen->cursor_x;

    if (cursor >= row_end) {
        cursor = row_end - 1;
    }

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

static void _redraw_screen(size_t index) {
    console_screen_t *screen = _get_screen(index);
    console_cell_t *cells = _screen_cells(index);

    if (!screen || !console_state.ready) {
        return;
    }

    if (console_state.mode == CONSOLE_TEXT && _screen_is_blank(index)) {
        return;
    }

    bool temp_batch = false;

    if (index == console_state.active_screen && _has_back_buffer() && !console_state.flush_batch) {
        console_state.flush_batch = true;
        temp_batch = true;
    }

    if (console_state.mode == CONSOLE_TEXT) {
        _clear_text(screen);
    } else if (console_state.mode == CONSOLE_FRAMEBUFFER) {
        _clear_fb(screen);
    }

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
            console_cell_t *cell = &cells[row * cols + col];
            u32 codepoint = cell->codepoint ? cell->codepoint : ' ';

            if (console_state.mode == CONSOLE_TEXT) {
                _draw_char_text(codepoint, col, row, cell->fg, cell->bg);
            } else if (console_state.mode == CONSOLE_FRAMEBUFFER) {
                u32 fg = ansi_color_rgb(cell->fg);
                u32 bg = ansi_color_rgb(cell->bg);
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
    bool notify_ws = false;

    unsigned long irq_flags = spin_lock_irqsave(&console_lock);

    if (!console_state.ready) {
        spin_unlock_irqrestore(&console_lock, irq_flags);
        return false;
    }

    if (index >= console_state.screen_count) {
        spin_unlock_irqrestore(&console_lock, irq_flags);
        return false;
    }

    size_t previous = console_state.active_screen;
    bool screen_changed = (console_state.active_screen != index);
    if (screen_changed) {
        console_state.active_screen = index;
    }

    if (console_state.fb_owned && index == console_state.fb_owner_screen) {
        notify_ws = true;
    } else if (
        console_state.mode == CONSOLE_TEXT &&
        screen_changed &&
        previous == TTY_CONSOLE &&
        _screen_is_blank(index)
    ) {
        _inherit_screen_state(index, previous);
        console_state.handoff_refresh_pending = false;
    } else if (screen_changed || console_state.handoff_refresh_pending) {
        // Ensure the first tty-facing activation cannot inherit stale pixels.
        console_state.cursor_drawn = false;
        console_state.dirty = false;
        _redraw_screen(index);
        console_state.handoff_refresh_pending = false;
    }

    spin_unlock_irqrestore(&console_lock, irq_flags);

    if (notify_ws) {
        ws_notify_screen_active();
    }

    return true;
}

static void _scroll_screen(console_screen_t *screen, size_t screen_index) {
    if (!screen) {
        return;
    }

    console_cell_t *cells = _screen_cells(screen_index);
    size_t cols = console_state.cols;
    size_t rows = console_state.rows;

    if (cells && cols && rows) {
        term_cells_scroll_up(
            cells, cols, rows, screen->color.fg_idx, screen->color.bg_idx
        );
    }

    if (screen_index != console_state.active_screen) {
        return;
    }

    if (console_state.mode == CONSOLE_TEXT) {
        _scroll_text(screen);
    } else if (console_state.mode == CONSOLE_FRAMEBUFFER) {
        _scroll_fb(screen);
    }
}

static void _newline(console_screen_t *screen, size_t screen_index) {
    screen->cursor_x = 0;
    screen->cursor_y++;

    if (screen->cursor_y < console_state.rows) {
        return;
    }

    screen->cursor_y = console_state.rows - 1;
    _scroll_screen(screen, screen_index);
}

static size_t _next_tab_stop(size_t cursor_x) {
    size_t next = ((cursor_x / CONSOLE_TAB_WIDTH) + 1) * CONSOLE_TAB_WIDTH;
    return next < console_state.cols ? next : console_state.cols;
}

static void _putc(console_screen_t *screen, size_t screen_index, u32 ch) {
    if (!screen || !console_state.ready || console_state.mode == CONSOLE_DISABLED) {
        return;
    }

    if (!console_state.cols || !console_state.rows) {
        return;
    }

    if (screen_index == console_state.active_screen) {
        _cursor_hide();
    }

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

        while (screen->cursor_x < next) {
            _putc(screen, screen_index, ' ');
        }

        _cursor_show(screen_index);
        return;
    }

    if (ch == '\b') {
        if (screen->cursor_x > 0) {
            screen->cursor_x--;

            size_t col = screen->cursor_x;
            size_t row = screen->cursor_y;

            console_cell_t *cells = _screen_cells(screen_index);

            if (cells) {
                console_cell_t *cell = &cells[row * console_state.cols + col];
                term_cell_set_blank(
                    cell, screen->color.fg_idx, screen->color.bg_idx
                );
            }

            if (screen_index == console_state.active_screen) {
                if (console_state.mode == CONSOLE_TEXT) {
                    _draw_char_text(
                        ' ',
                        col,
                        row,
                        screen->color.fg_idx,
                        screen->color.bg_idx
                    );
                } else if (console_state.mode == CONSOLE_FRAMEBUFFER) {
                    _draw_char_fb(
                        ' ',
                        col,
                        row,
                        ansi_color_rgb(screen->color.fg_idx),
                        ansi_color_rgb(screen->color.bg_idx)
                    );
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

    if (screen->cursor_x >= console_state.cols) {
        _newline(screen, screen_index);
    }

    size_t col = screen->cursor_x;
    size_t row = screen->cursor_y;

    console_cell_t *cells = _screen_cells(screen_index);

    if (cells) {
        console_cell_t *cell = &cells[row * console_state.cols + col];
        cell->codepoint = ch;
        cell->fg = screen->color.fg_idx;
        cell->bg = screen->color.bg_idx;
    }

    if (screen_index == console_state.active_screen) {
        if (console_state.mode == CONSOLE_TEXT) {
            _draw_char_text(
                ch, col, row, screen->color.fg_idx, screen->color.bg_idx
            );
        } else if (console_state.mode == CONSOLE_FRAMEBUFFER) {
            _draw_char_fb(
                ch,
                col,
                row,
                ansi_color_rgb(screen->color.fg_idx),
                ansi_color_rgb(screen->color.bg_idx)
            );
        }
    }

    screen->cursor_x++;

    _cursor_show(screen_index);
}

static void _utf8_emit_codepoint(void *opaque, u32 codepoint) {
    console_ansi_ctx_t *ctx = opaque;
    if (!ctx || !ctx->screen) {
        return;
    }

    _putc(ctx->screen, ctx->screen_index, codepoint);
}

static void _utf8_emit_invalid(void *opaque) {
    console_ansi_ctx_t *ctx = opaque;
    if (!ctx || !ctx->screen) {
        return;
    }

    _putc(ctx->screen, ctx->screen_index, '?');
}

static const term_utf8_callbacks_t _utf8_callbacks = {
    .on_codepoint = _utf8_emit_codepoint,
    .on_invalid = _utf8_emit_invalid,
};

static void _flush_invalid_utf8(console_ansi_ctx_t *ctx) {
    if (!ctx || !ctx->screen) {
        return;
    }

    term_utf8_flush_invalid(&ctx->screen->utf8, &_utf8_callbacks, ctx);
}

static void _ansi_print(void *opaque, u8 ch) {
    console_ansi_ctx_t *ctx = opaque;
    if (!ctx || !ctx->screen) {
        return;
    }

    term_utf8_feed(&ctx->screen->utf8, ch, &_utf8_callbacks, ctx);
}

static void _ansi_control(void *opaque, u8 ch) {
    console_ansi_ctx_t *ctx = opaque;
    if (!ctx || !ctx->screen) {
        return;
    }

    _flush_invalid_utf8(ctx);
    _putc(ctx->screen, ctx->screen_index, ch);
}

static void _ansi_csi_cursor_show(void *opaque) {
    console_ansi_ctx_t *ctx = opaque;
    if (!ctx || !ctx->screen) {
        return;
    }

    _cursor_show(ctx->screen_index);
}

static void _ansi_csi_cursor_hide(void *opaque) {
    console_ansi_ctx_t *ctx = opaque;
    if (!ctx || !ctx->screen) {
        return;
    }

    _cursor_hide();
}

static void _ansi_csi(
    void *opaque,
    char op,
    const int *params,
    size_t count,
    bool private_mode
) {
    console_ansi_ctx_t *ctx = opaque;
    if (!ctx || !ctx->screen) {
        return;
    }

    ansi_csi_state_t csi_state = {
        .cursor_x = &ctx->screen->cursor_x,
        .cursor_y = &ctx->screen->cursor_y,
        .saved_x = &ctx->screen->saved_cursor_x,
        .saved_y = &ctx->screen->saved_cursor_y,
        .saved_valid = &ctx->screen->saved_cursor_valid,
        .cursor_visible = NULL,
        .cols = console_state.cols,
        .rows = console_state.rows,
        .color = &ctx->screen->color,
        .clear_screen = _handle_csi_clear,
        .clear_line = _handle_csi_clear_line,
        .cursor_show = _ansi_csi_cursor_show,
        .cursor_hide = _ansi_csi_cursor_hide,
        .ctx = ctx,
    };

    ansi_csi_dispatch_state(op, params, count, private_mode, &csi_state);
}

static const ansi_callbacks_t _ansi_callbacks = {
    .on_print = _ansi_print,
    .on_control = _ansi_control,
    .on_csi = _ansi_csi,
    .on_escape = NULL,
};

static void
_write_screen_locked(size_t screen_index, const char *buf, size_t len) {
    if (!buf || !len || !console_state.ready || console_state.mode == CONSOLE_DISABLED) {
        return;
    }

    console_screen_t *screen = _get_screen(screen_index);
    if (!screen) {
        return;
    }

    // Until a font-backed text grid exists, ignore screen writes. Boot logs are
    // still preserved in arch log history and replayed once the grid is ready.
    if (!console_state.cols || !console_state.rows) {
        return;
    }

    bool post_handoff_refresh = false;

    if (
        screen_index == console_state.active_screen &&
        console_state.handoff_refresh_pending &&
        !(console_state.fb_owned && screen_index == console_state.fb_owner_screen)
    ) {
        // Post-handoff first write: force a full repaint before incremental draws.
        console_state.cursor_drawn = false;
        console_state.dirty = false;
        _redraw_screen(screen_index);
        // Keep pending set until we also repaint once after the write batch.
        post_handoff_refresh = true;
    }

    bool batch_cursor = (screen_index == console_state.active_screen);
    bool batch_flush = batch_cursor && _has_back_buffer();

    if (batch_cursor) {
        _cursor_hide();
        console_state.cursor_batch = true;
    }

    if (batch_flush) {
        console_state.flush_batch = true;
    }

    console_ansi_ctx_t ansi_ctx = {
        .screen_index = screen_index,
        .screen = screen,
    };

    for (size_t i = 0; i < len; i++) {
        u8 ch = (u8)buf[i];

        if (ch == '\x1b') {
            _flush_invalid_utf8(&ansi_ctx);
        }

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

    if (post_handoff_refresh) {
        // One more full repaint after the first write batch ensures no
        // incremental-path artifact survives (same effect as tty switch+back).
        console_state.cursor_drawn = false;
        console_state.dirty = false;
        _redraw_screen(screen_index);
        console_state.handoff_refresh_pending = false;
    }
}

static void
_clamp_screen_positions(console_screen_t *screen, size_t cols, size_t rows) {
    if (!screen) {
        return;
    }

    term_cursor_clamp(
        &screen->cursor_x,
        &screen->cursor_y,
        &screen->saved_cursor_x,
        &screen->saved_cursor_y,
        &screen->saved_cursor_valid,
        cols,
        rows
    );
}

static void _preserve_screens(
    const console_screen_t *old_screens,
    size_t old_screen_count,
    const console_cell_t *old_cells,
    size_t old_cols,
    size_t old_rows
) {
    if (!old_screens || !console_state.screens || !console_state.screen_count) {
        return;
    }

    size_t copy_screens = min(old_screen_count, console_state.screen_count);
    size_t new_cols = console_state.cols;
    size_t new_rows = console_state.rows;
    size_t copy_cols = min(old_cols, new_cols);
    size_t copy_rows = min(old_rows, new_rows);
    size_t old_stride = old_cols * old_rows;
    size_t new_stride = _cell_count();

    for (size_t i = 0; i < copy_screens; i++) {
        console_screen_t *dst_screen = &console_state.screens[i];
        const console_screen_t *src_screen = &old_screens[i];

        *dst_screen = *src_screen;
        _clamp_screen_positions(dst_screen, new_cols, new_rows);

        if (dst_screen->utf8.pending_len > sizeof(dst_screen->utf8.pending)) {
            term_utf8_reset(&dst_screen->utf8);
        }

        if (
            !old_cells ||
            !console_state.cells ||
            !copy_cols ||
            !copy_rows ||
            !old_stride ||
            !new_stride
        ) {
            continue;
        }

        const console_cell_t *src_cells = old_cells + i * old_stride;
        console_cell_t *dst_cells = console_state.cells + i * new_stride;

        for (size_t row = 0; row < copy_rows; row++) {
            memcpy(
                dst_cells + row * new_cols,
                src_cells + row * old_cols,
                copy_cols * sizeof(*dst_cells)
            );
        }
    }
}

static void _init_screens(size_t active_screen) {
    console_state.screen_count = TTY_SCREEN_COUNT;
    if (!console_state.screen_count) {
        console_state.screen_count = 1;
    }

    if (active_screen >= console_state.screen_count) {
        active_screen = TTY_CONSOLE;
    }

    size_t count = _cell_count();

    console_state.screens =
        calloc(console_state.screen_count, sizeof(console_screen_t));

    if (!console_state.screens) {
        console_state.screen_count = 1;
        console_state.screens = &console_state.fallback_screen;
        console_state.cells = NULL;
        console_state.active_screen = 0;

        _screen_reset(&console_state.fallback_screen);

        return;
    }

    if (!count) {
        console_state.cells = NULL;
        console_state.active_screen = active_screen;

        for (size_t i = 0; i < console_state.screen_count; i++) {
            _screen_reset(&console_state.screens[i]);
        }

        return;
    }

    console_state.cells =
        calloc(console_state.screen_count * count, sizeof(console_cell_t));

    if (!console_state.cells) {
        if (console_state.screens) {
            free(console_state.screens);
        }

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

void console_set_font(const font_t *font) {
    if (!font) {
        return;
    }

    if (!font->glyphs || !font->glyph_width || !font->glyph_height) {
        return;
    }

    if (console_state.mode == CONSOLE_FRAMEBUFFER && console_state.width && console_state.height) {
        size_t cols = console_state.width / font->glyph_width;
        size_t rows = console_state.height / font->glyph_height;

        if (!cols || !rows) {
            return;
        }
    }

    unsigned long irq_flags = spin_lock_irqsave(&console_lock);

    size_t old_cols = console_state.cols;
    size_t old_rows = console_state.rows;

    _use_font(font);

    if (!console_state.ready || console_state.mode != CONSOLE_FRAMEBUFFER) {
        spin_unlock_irqrestore(&console_lock, irq_flags);
        return;
    }

    console_state.cols = console_state.width / console_state.font_cell_width;
    console_state.rows = console_state.height / console_state.font_cell_height;


    if (
        console_state.screens &&
        console_state.cells &&
        console_state.cols == old_cols &&
        console_state.rows == old_rows
    ) {
        _redraw_screen(console_state.active_screen);
        spin_unlock_irqrestore(&console_lock, irq_flags);
        return;
    }

    console_screen_t *old_screens = console_state.screens;
    console_cell_t *old_cells = console_state.cells;
    size_t old_screen_count = console_state.screen_count;

    bool free_old_screens =
        old_screens && old_screens != &console_state.fallback_screen;
    bool free_old_cells = old_cells != NULL;

    size_t active = console_state.active_screen;

    console_state.screens = NULL;
    console_state.cells = NULL;
    console_state.screen_count = 0;

    _init_screens(active);
    _preserve_screens(
        old_screens, old_screen_count, old_cells, old_cols, old_rows
    );

    if (free_old_screens) {
        free(old_screens);
    }
    if (free_old_cells) {
        free(old_cells);
    }

    _redraw_screen(console_state.active_screen);
    spin_unlock_irqrestore(&console_lock, irq_flags);
}

void console_init(void *arch_boot_info) {
    memset(&console_state, 0, sizeof(console_state));

    if (!backend_ops || !backend_ops->probe) {
        return;
    }

    console_hw_desc_t hw = {0};
    if (!backend_ops->probe(arch_boot_info, &hw)) {
        return;
    }

    if (hw.mode == CONSOLE_DISABLED) {
        return;
    }

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

        if (console_state.font_width && console_state.font_height) {
            console_state.cols = console_state.width / console_state.font_cell_width;
            console_state.rows = console_state.height / console_state.font_cell_height;
        } else {
            console_state.cols = 0;
            console_state.rows = 0;
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
    console_state.handoff_refresh_pending = true;
    _init_screens(TTY_CONSOLE);
    _redraw_screen(console_state.active_screen);
}

ssize_t console_read(void *buf, size_t len) {
    if (!buf) {
        return -EFAULT;
    }

    if (!len) {
        return 0;
    }

    return -ENOSYS;
}

ssize_t console_write_screen(size_t screen, const void *buf, size_t len) {
    if (!buf) {
        return -EFAULT;
    }

    if (!len) {
        return 0;
    }

    unsigned long flags = spin_lock_irqsave(&console_lock);
    _write_screen_locked(screen, buf, len);
    spin_unlock_irqrestore(&console_lock, flags);

    return (ssize_t)len;
}

ssize_t console_write(const void *buf, size_t len) {
    return console_write_screen(TTY_CONSOLE, buf, len);
}

bool console_get_size(size_t *cols, size_t *rows) {
    if (!cols || !rows) {
        return false;
    }

    unsigned long irq_flags = spin_lock_irqsave(&console_lock);

    if (!console_state.ready || console_state.mode == CONSOLE_DISABLED) {
        spin_unlock_irqrestore(&console_lock, irq_flags);
        return false;
    }

    *cols = console_state.cols;
    *rows = console_state.rows;

    spin_unlock_irqrestore(&console_lock, irq_flags);
    return true;
}

int console_fb_acquire(pid_t pid, size_t screen) {
    if (pid <= 0) {
        return -EINVAL;
    }

    if (!console_state.ready || console_state.mode != CONSOLE_FRAMEBUFFER || !_has_back_buffer()) {
        return -ENODEV;
    }

    if (screen >= console_state.screen_count) {
        return -EINVAL;
    }

    unsigned long irq_flags = spin_lock_irqsave(&console_lock);

    if (console_state.fb_owned && console_state.fb_owner != pid) {
        spin_unlock_irqrestore(&console_lock, irq_flags);
        return -EBUSY;
    }

    console_state.fb_owned = true;
    console_state.fb_owner = pid;
    console_state.fb_owner_screen = screen;

    spin_unlock_irqrestore(&console_lock, irq_flags);
    return 0;
}

int console_fb_release(pid_t pid) {
    if (pid <= 0) {
        return -EINVAL;
    }

    if (!console_state.ready || console_state.mode != CONSOLE_FRAMEBUFFER || !_has_back_buffer()) {
        return -ENODEV;
    }

    unsigned long irq_flags = spin_lock_irqsave(&console_lock);

    if (!console_state.fb_owned) {
        spin_unlock_irqrestore(&console_lock, irq_flags);
        return 0;
    }

    if (console_state.fb_owner != pid) {
        spin_unlock_irqrestore(&console_lock, irq_flags);
        return -EPERM;
    }

    console_state.fb_owned = false;
    console_state.fb_owner = 0;
    console_state.fb_owner_screen = TTY_CONSOLE;
    console_state.dirty = false;
    console_state.handoff_refresh_pending = true;

    _redraw_screen(console_state.active_screen);

    spin_unlock_irqrestore(&console_lock, irq_flags);
    return 0;
}

ssize_t console_fb_owner_screen(void) {
    if (
        !console_state.ready ||
        console_state.mode != CONSOLE_FRAMEBUFFER ||
        !_has_back_buffer()
    ) {
        return TTY_NONE;
    }

    unsigned long irq_flags = spin_lock_irqsave(&console_lock);

    ssize_t owner_screen =
        console_state.fb_owned ? (ssize_t)console_state.fb_owner_screen : TTY_NONE;

    spin_unlock_irqrestore(&console_lock, irq_flags);

    return owner_screen;
}

void console_panic(void) {
    if (!console_state.ready || console_state.mode == CONSOLE_DISABLED) {
        return;
    }

    unsigned long irq_flags = spin_lock_irqsave(&console_lock);

    if (console_state.mode == CONSOLE_FRAMEBUFFER && _has_back_buffer()) {
        console_state.fb_owned = false;
        console_state.fb_owner = 0;
        console_state.fb_owner_screen = TTY_CONSOLE;
        console_state.flush_batch = false;
        console_state.dirty = false;
    }

    if (console_state.active_screen != TTY_CONSOLE) {
        console_state.active_screen = TTY_CONSOLE;
    }

    _redraw_screen(TTY_CONSOLE);

    spin_unlock_irqrestore(&console_lock, irq_flags);
}
