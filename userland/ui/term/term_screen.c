#include "term_screen.h"

#include <draw.h>
#include <psf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <term/ansi.h>
#include <term/cells.h>
#include <term/cursor.h>
#include <term/glyph.h>
#include <term/utf8.h>

#define FONT_BUF_SIZE  (256 * 1024)
#define TERM_TAB_WIDTH 8
#define TERM_SCROLLBACK_LINES          4096
#define TERM_SCROLLBAR_WIDTH_PX        3
#define TERM_SCROLLBAR_MIN_THUMB_PX    8
#define TERM_SCROLLBAR_TRACK_COLOR     0x00262626U
#define TERM_SCROLLBAR_THUMB_COLOR     0x00545454U
#define TERM_SCROLLBAR_THUMB_ACTIVE    0x007a7a7aU

typedef struct {
    u32 codepoint;
    u32 glyph;
} term_font_map_entry_t;

typedef struct {
    psf_font_t font;
    term_font_map_entry_t *font_map;
    size_t font_map_count;
    size_t font_map_capacity;
    bool font_map_ready;
    u32 cell_width;
    u32 cell_height;
    u32 cell_src_x;
    term_cell_t cells[TERM_MAX_ROWS * TERM_MAX_COLS];
    term_cell_t history[TERM_SCROLLBACK_LINES * TERM_MAX_COLS];
    u8 *font_buf;
    pixel_t *pixels;
    size_t pixels_count;
    u32 width;
    u32 height;
    u32 stride;
    u32 scrollbar_width;
    size_t cols;
    size_t rows;
    size_t history_head;
    size_t history_count;
    size_t scroll_offset;
    size_t cursor_x;
    size_t cursor_y;
    size_t saved_x;
    size_t saved_y;
    bool cursor_visible;
    bool cursor_prev_valid;
    size_t cursor_prev_x;
    size_t cursor_prev_y;
    bool dirty;
    size_t dirty_x0;
    size_t dirty_y0;
    size_t dirty_x1;
    size_t dirty_y1;
    bool scrollbar_dirty;
    term_utf8_state_t utf8;
    ansi_parser_t ansi;
    ansi_color_state_t color;
    bool ready;
} term_screen_state_t;

static term_screen_state_t term_screen = {0};

static framebuffer_t _screen_framebuffer(void) {
    framebuffer_t fb = {
        .pixels = term_screen.pixels,
        .width = term_screen.width,
        .height = term_screen.height,
        .stride = term_screen.stride,
        .pixel_count = term_screen.pixels_count,
    };
    return fb;
}

static size_t max_scroll_offset(void) {
    return term_screen.history_count;
}

static size_t clamped_scroll_offset(void) {
    size_t max = max_scroll_offset();
    if (term_screen.scroll_offset > max) {
        term_screen.scroll_offset = max;
    }
    return term_screen.scroll_offset;
}

static void clear_history(void) {
    term_screen.history_head = 0;
    term_screen.history_count = 0;
    term_screen.scroll_offset = 0;
    term_screen.scrollbar_dirty = true;
}

static term_cell_t *history_line_at(size_t logical_index) {
    if (logical_index >= term_screen.history_count) {
        return NULL;
    }

    size_t physical =
        (term_screen.history_head + logical_index) % TERM_SCROLLBACK_LINES;
    return &term_screen.history[physical * TERM_MAX_COLS];
}

static const term_cell_t *history_line_c(size_t logical_index) {
    return history_line_at(logical_index);
}

static void mark_dirty_minimal(void) {
    if (!term_screen.cols || !term_screen.rows || term_screen.dirty) {
        return;
    }

    term_screen.dirty = true;
    term_screen.dirty_x0 = term_screen.cols - 1;
    term_screen.dirty_y0 = 0;
    term_screen.dirty_x1 = term_screen.cols;
    term_screen.dirty_y1 = 1;
}

static void push_history_row(const term_cell_t *row) {
    if (!row || !term_screen.cols) {
        return;
    }

    size_t physical = 0;
    if (term_screen.history_count < TERM_SCROLLBACK_LINES) {
        physical =
            (term_screen.history_head + term_screen.history_count) %
            TERM_SCROLLBACK_LINES;
        term_screen.history_count++;
    } else {
        physical = term_screen.history_head;
        term_screen.history_head =
            (term_screen.history_head + 1) % TERM_SCROLLBACK_LINES;
    }

    term_cell_t *dst = &term_screen.history[physical * TERM_MAX_COLS];
    term_cells_clear_range(
        dst,
        TERM_MAX_COLS,
        0,
        TERM_MAX_COLS,
        term_screen.color.fg_idx,
        term_screen.color.bg_idx
    );
    memcpy(dst, row, term_screen.cols * sizeof(*dst));

    size_t max = max_scroll_offset();
    if (term_screen.scroll_offset > 0 && term_screen.scroll_offset < max) {
        term_screen.scroll_offset++;
    } else if (term_screen.scroll_offset > max) {
        term_screen.scroll_offset = max;
    }

    term_screen.scrollbar_dirty = true;
    if (term_screen.scroll_offset > 0) {
        mark_dirty_minimal();
    }
}

static const term_cell_t *view_row_cells(size_t view_row) {
    if (view_row >= term_screen.rows) {
        return NULL;
    }

    size_t offset = clamped_scroll_offset();
    size_t start_history = term_screen.history_count;
    if (offset < start_history) {
        start_history -= offset;
    } else {
        start_history = 0;
    }

    size_t line_index = start_history + view_row;
    if (line_index < term_screen.history_count) {
        return history_line_c(line_index);
    }

    size_t live_row = line_index - term_screen.history_count;
    if (live_row >= term_screen.rows) {
        return NULL;
    }

    return &term_screen.cells[live_row * term_screen.cols];
}

static void clear_pixels_full(void) {
    if (!term_screen.pixels || !term_screen.width || !term_screen.height) {
        return;
    }

    u32 bg = ansi_color_rgb(term_screen.color.bg_idx);
    framebuffer_t fb = _screen_framebuffer();
    draw_rect(&fb, 0, 0, term_screen.width, term_screen.height, bg);
}

static void clear_font_map(void) {
    if (term_screen.font_map) {
        free(term_screen.font_map);
        term_screen.font_map = NULL;
    }

    term_screen.font_map_count = 0;
    term_screen.font_map_capacity = 0;
    term_screen.font_map_ready = false;
}

static bool reserve_font_map(size_t needed) {
    if (term_screen.font_map_capacity >= needed) {
        return true;
    }

    size_t new_cap = 128;
    if (term_screen.font_map_capacity) {
        new_cap = term_screen.font_map_capacity * 2;
    }

    while (new_cap < needed) {
        new_cap *= 2;
    }

    term_font_map_entry_t *next = malloc(new_cap * sizeof(*next));
    if (!next) {
        return false;
    }

    if (term_screen.font_map_count && term_screen.font_map) {
        memcpy(
            next,
            term_screen.font_map,
            term_screen.font_map_count * sizeof(*next)
        );
    }

    if (term_screen.font_map) {
        free(term_screen.font_map);
    }

    term_screen.font_map = next;
    term_screen.font_map_capacity = new_cap;
    return true;
}

static bool push_font_map(u32 codepoint, u32 glyph) {
    if (codepoint == 0xfffeU || codepoint == 0xffffU) {
        return true;
    }

    if (!reserve_font_map(term_screen.font_map_count + 1)) {
        return false;
    }

    term_screen.font_map[term_screen.font_map_count++] = (term_font_map_entry_t){
        .codepoint = codepoint,
        .glyph = glyph,
    };
    return true;
}

static bool push_font_map_iter(void *ctx, u32 codepoint, u32 glyph) {
    (void)ctx;
    return push_font_map(codepoint, glyph);
}

static void sort_font_map(void) {
    for (size_t i = 1; i < term_screen.font_map_count; i++) {
        term_font_map_entry_t current = term_screen.font_map[i];
        size_t j = i;

        while (j > 0 && term_screen.font_map[j - 1].codepoint > current.codepoint) {
            term_screen.font_map[j] = term_screen.font_map[j - 1];
            j--;
        }

        term_screen.font_map[j] = current;
    }
}

static bool find_mapped_glyph(u32 codepoint, u32 *glyph_out) {
    if (!glyph_out || !term_screen.font_map_count || !term_screen.font_map) {
        return false;
    }

    size_t lo = 0;
    size_t hi = term_screen.font_map_count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const term_font_map_entry_t *entry = &term_screen.font_map[mid];

        if (entry->codepoint == codepoint) {
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

static u32 glyph_index_for(u32 codepoint) {
    u32 glyph = 0;

    // PSF2 fonts may have sparse/non-linear Unicode mappings.
    // Prefer explicit table lookup whenever available.
    if (find_mapped_glyph(codepoint, &glyph)) {
        if (glyph < term_screen.font.glyph_count) {
            return glyph;
        }
    }

    if (codepoint < term_screen.font.glyph_count) {
        return codepoint;
    }

    if (find_mapped_glyph((u32)'?', &glyph)) {
        if (glyph < term_screen.font.glyph_count) {
            return glyph;
        }
    }

    if ((u32)'?' < term_screen.font.glyph_count) {
        return (u32)'?';
    }

    return 0;
}

static bool glyph_pixel_on(const u8 *glyph, u32 row_bytes, u32 x, u32 y) {
    const u8 *row = glyph + (size_t)y * row_bytes;
    u8 bits = row[x / 8];
    u8 mask = (u8)(0x80 >> (x & 7));
    return (bits & mask) != 0;
}

static void set_default_cell_metrics(void) {
    term_screen.cell_src_x = 0;
    term_screen.cell_width = term_screen.font.width ? term_screen.font.width : 1;
    term_screen.cell_height = term_screen.font.height ? term_screen.font.height : 1;
}

static bool glyph_bounds_for_index(u32 glyph_idx, u32 *left_out, u32 *right_out) {
    if (
        glyph_idx >= term_screen.font.glyph_count ||
        !left_out ||
        !right_out
    ) {
        return false;
    }

    const u8 *glyph =
        term_screen.font.glyphs + (size_t)glyph_idx * term_screen.font.glyph_size;
    u32 left = term_screen.font.width;
    u32 right = 0;
    bool any = false;

    for (u32 gy = 0; gy < term_screen.font.height; gy++) {
        for (u32 gx = 0; gx < term_screen.font.width; gx++) {
            if (!glyph_pixel_on(glyph, term_screen.font.row_bytes, gx, gy)) {
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

static void derive_cell_metrics(void) {
    set_default_cell_metrics();

    if (
        !term_screen.font.glyphs ||
        !term_screen.font.width ||
        !term_screen.font.height
    ) {
        return;
    }

    u32 min_left = term_screen.font.width;
    u32 max_right = 0;
    bool have_bounds = false;

    // Derive terminal cell advance from printable ASCII glyphs.
    // This matches monospace terminal expectations and avoids wider
    // extended symbols inflating the cell width.
    for (u32 cp = 32; cp < 127; cp++) {
        u32 glyph = 0;
        if (term_screen.font_map_count) {
            if (!find_mapped_glyph(cp, &glyph)) {
                continue;
            }
        } else {
            glyph = glyph_index_for(cp);
        }

        u32 left = 0;
        u32 right = 0;
        if (!glyph_bounds_for_index(glyph, &left, &right)) {
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

    u32 ink_span = max_right - min_left + 1;
    u32 max_advance = term_screen.font.width - min_left;

    if (!ink_span || !max_advance) {
        return;
    }

    u32 advance = ink_span;
    if (!advance) {
        return;
    }

    if (advance < max_advance) {
        u32 padded = advance + TERM_GLYPH_CELL_GAP_PX;
        if (padded < advance || padded > max_advance) {
            padded = max_advance;
        }
        advance = padded;
    }

    term_screen.cell_src_x = min_left;
    term_screen.cell_width = advance;
}

static bool build_font_map(void) {
    clear_font_map();

    if (
        !psf_iter_unicode_mappings(
            &term_screen.font, push_font_map_iter, NULL
        )
    ) {
        clear_font_map();
        return false;
    }

    sort_font_map();
    term_screen.font_map_ready = true;
    return true;
}

static bool init_font(void) {
    if (term_screen.font.glyphs && term_screen.font_map_ready) {
        derive_cell_metrics();
        return true;
    }

    if (!term_screen.font_buf) {
        term_screen.font_buf = malloc(FONT_BUF_SIZE);
        if (!term_screen.font_buf) {
            return false;
        }
    }

    const char *font_path = draw_get_font_path();

    if (
        !term_screen.font.glyphs &&
        !psf_load_file(
            font_path,
            term_screen.font_buf,
            FONT_BUF_SIZE,
            &term_screen.font
        )
    ) {
        return false;
    }

    if (!build_font_map()) {
        return false;
    }

    derive_cell_metrics();
    return true;
}

static void mark_dirty_rect(size_t x0, size_t y0, size_t x1, size_t y1) {
    if (!term_screen.cols || !term_screen.rows || x0 >= x1 || y0 >= y1) {
        return;
    }

    // While detached from bottom, keep viewport stable and avoid
    // repaint churn from incoming PTY output.
    if (term_screen.scroll_offset > 0) {
        return;
    }

    if (x0 >= term_screen.cols || y0 >= term_screen.rows) {
        return;
    }

    if (x1 > term_screen.cols) {
        x1 = term_screen.cols;
    }

    if (y1 > term_screen.rows) {
        y1 = term_screen.rows;
    }

    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    if (!term_screen.dirty) {
        term_screen.dirty = true;
        term_screen.dirty_x0 = x0;
        term_screen.dirty_y0 = y0;
        term_screen.dirty_x1 = x1;
        term_screen.dirty_y1 = y1;
        return;
    }

    if (x0 < term_screen.dirty_x0) {
        term_screen.dirty_x0 = x0;
    }

    if (y0 < term_screen.dirty_y0) {
        term_screen.dirty_y0 = y0;
    }

    if (x1 > term_screen.dirty_x1) {
        term_screen.dirty_x1 = x1;
    }

    if (y1 > term_screen.dirty_y1) {
        term_screen.dirty_y1 = y1;
    }
}

static void mark_dirty_cell(size_t x, size_t y) {
    mark_dirty_rect(x, y, x + 1, y + 1);
}

static void mark_dirty_all(void) {
    if (!term_screen.cols || !term_screen.rows) {
        return;
    }

    term_screen.dirty = true;
    term_screen.dirty_x0 = 0;
    term_screen.dirty_y0 = 0;
    term_screen.dirty_x1 = term_screen.cols;
    term_screen.dirty_y1 = term_screen.rows;
}

static void mark_cursor_move(size_t old_x, size_t old_y) {
    if (old_x < term_screen.cols && old_y < term_screen.rows) {
        mark_dirty_cell(old_x, old_y);
    }

    if (term_screen.cursor_x < term_screen.cols && term_screen.cursor_y < term_screen.rows) {
        mark_dirty_cell(term_screen.cursor_x, term_screen.cursor_y);
    }
}

static term_cell_t *cell_buffer(void) {
    return term_screen.cells;
}

static size_t cell_count(void) {
    return term_screen.cols * term_screen.rows;
}

static term_cell_t *cell_at(size_t x, size_t y) {
    return &term_screen.cells[y * term_screen.cols + x];
}

static void clear_cells(void) {
    term_cells_clear(
        cell_buffer(),
        term_screen.cols,
        term_screen.rows,
        term_screen.color.fg_idx,
        term_screen.color.bg_idx
    );
}

static void scroll_up(void) {
    bool detached = term_screen.scroll_offset > 0;

    push_history_row(cell_buffer());
    term_cells_scroll_up(
        cell_buffer(),
        term_screen.cols,
        term_screen.rows,
        term_screen.color.fg_idx,
        term_screen.color.bg_idx
    );

    if (!detached) {
        mark_dirty_all();
    }
}

static void newline(void) {
    term_screen.cursor_x = 0;
    term_screen.cursor_y++;

    if (term_screen.cursor_y < term_screen.rows) {
        return;
    }

    term_screen.cursor_y = term_screen.rows - 1;
    scroll_up();
}

static size_t next_tab_stop(size_t cursor_x) {
    return ((cursor_x / TERM_TAB_WIDTH) + 1) * TERM_TAB_WIDTH;
}

static void put_codepoint(u32 codepoint) {
    size_t old_x = term_screen.cursor_x;
    size_t old_y = term_screen.cursor_y;

    if (codepoint == '\r') {
        term_screen.cursor_x = 0;
        mark_cursor_move(old_x, old_y);
        return;
    }

    if (codepoint == '\n') {
        newline();
        mark_cursor_move(old_x, old_y);
        return;
    }

    if (codepoint == '\b') {
        if (term_screen.cursor_x > 0) {
            term_screen.cursor_x--;
        }
        mark_cursor_move(old_x, old_y);
        return;
    }

    if (codepoint == '\t') {
        size_t next = next_tab_stop(term_screen.cursor_x);

        while (term_screen.cursor_x < next) {
            put_codepoint(' ');
        }

        return;
    }

    if (codepoint < 32) {
        return;
    }

    if (term_screen.cursor_x >= term_screen.cols) {
        newline();
    }

    term_cell_t *cell = cell_at(term_screen.cursor_x, term_screen.cursor_y);

    if (
        cell->codepoint != codepoint ||
        cell->fg != term_screen.color.fg_idx ||
        cell->bg != term_screen.color.bg_idx
    ) {
        cell->codepoint = codepoint;
        cell->fg = term_screen.color.fg_idx;
        cell->bg = term_screen.color.bg_idx;
        mark_dirty_cell(term_screen.cursor_x, term_screen.cursor_y);
    }

    term_screen.cursor_x++;

    mark_cursor_move(old_x, old_y);
}

static void clear_line_mode(void *ctx, int mode) {
    (void)ctx;

    if (!term_screen.cols || !term_screen.rows) {
        return;
    }

    term_cell_t *cells = cell_buffer();
    size_t count = cell_count();
    size_t y = term_screen.cursor_y;
    size_t row_start = y * term_screen.cols;
    size_t row_end = row_start + term_screen.cols;

    if (mode == 2) {
        term_cells_clear_range(
            cells,
            count,
            row_start,
            row_end,
            term_screen.color.fg_idx,
            term_screen.color.bg_idx
        );

        mark_dirty_rect(0, y, term_screen.cols, y + 1);
        return;
    }

    if (mode == 1) {
        size_t end = term_screen.cursor_x + 1;
        if (end > term_screen.cols) {
            end = term_screen.cols;
        }

        term_cells_clear_range(
            cells,
            count,
            row_start,
            row_start + end,
            term_screen.color.fg_idx,
            term_screen.color.bg_idx
        );

        mark_dirty_rect(0, y, end, y + 1);
        return;
    }

    term_cells_clear_range(
        cells,
        count,
        row_start + term_screen.cursor_x,
        row_end,
        term_screen.color.fg_idx,
        term_screen.color.bg_idx
    );

    mark_dirty_rect(term_screen.cursor_x, y, term_screen.cols, y + 1);
}

static void clear_screen_mode(void *ctx, int mode) {
    (void)ctx;

    if (!term_screen.cols || !term_screen.rows) {
        return;
    }

    term_cell_t *cells = cell_buffer();
    size_t count = cell_count();
    size_t cursor = term_screen.cursor_y * term_screen.cols + term_screen.cursor_x;

    if (cursor > count) {
        cursor = count;
    }

    if (mode == 3) {
        clear_history();
        mark_dirty_all();
        return;
    }

    if (mode == 2) {
        clear_cells();
        term_screen.cursor_x = 0;
        term_screen.cursor_y = 0;
        term_screen.scroll_offset = 0;
        term_screen.scrollbar_dirty = true;
        mark_dirty_all();
        return;
    }

    if (mode == 1) {
        term_cells_clear_range(
            cells,
            count,
            0,
            cursor + 1,
            term_screen.color.fg_idx,
            term_screen.color.bg_idx
        );

        mark_dirty_rect(0, 0, term_screen.cols, term_screen.cursor_y + 1);
        return;
    }

    term_cells_clear_range(
        cells,
        count,
        cursor,
        count,
        term_screen.color.fg_idx,
        term_screen.color.bg_idx
    );

    mark_dirty_rect(
        0, term_screen.cursor_y, term_screen.cols, term_screen.rows
    );
}

static void utf8_on_codepoint(void *ctx, u32 codepoint) {
    (void)ctx;
    put_codepoint(codepoint);
}

static void utf8_on_invalid(void *ctx) {
    (void)ctx;
    put_codepoint('?');
}

static const term_utf8_callbacks_t term_utf8_callbacks = {
    .on_codepoint = utf8_on_codepoint,
    .on_invalid = utf8_on_invalid,
};

static void ansi_on_print(void *ctx, u8 ch) {
    (void)ctx;
    term_utf8_feed(&term_screen.utf8, ch, &term_utf8_callbacks, NULL);
}

static void ansi_on_control(void *ctx, u8 ch) {
    (void)ctx;
    term_utf8_flush_invalid(&term_screen.utf8, &term_utf8_callbacks, NULL);
    put_codepoint(ch);
}

static void ansi_on_csi(
    void *ctx,
    char op,
    const int *params,
    size_t count,
    bool private_mode
) {
    (void)ctx;

    size_t old_x = term_screen.cursor_x;
    size_t old_y = term_screen.cursor_y;

    ansi_csi_state_t csi_state = {
        .cursor_x = &term_screen.cursor_x,
        .cursor_y = &term_screen.cursor_y,
        .saved_x = &term_screen.saved_x,
        .saved_y = &term_screen.saved_y,
        .saved_valid = NULL,
        .cursor_visible = &term_screen.cursor_visible,
        .cols = term_screen.cols,
        .rows = term_screen.rows,
        .color = &term_screen.color,
        .clear_screen = clear_screen_mode,
        .clear_line = clear_line_mode,
        .cursor_show = NULL,
        .cursor_hide = NULL,
        .ctx = NULL,
    };

    ansi_csi_dispatch_state(op, params, count, private_mode, &csi_state);

    mark_cursor_move(old_x, old_y);
}

static const ansi_callbacks_t term_ansi_callbacks = {
    .on_print = ansi_on_print,
    .on_control = ansi_on_control,
    .on_csi = ansi_on_csi,
    .on_escape = NULL,
};

static const u8 *glyph_for(u32 codepoint) {
    u32 idx = glyph_index_for(codepoint);
    return term_screen.font.glyphs + (size_t)idx * term_screen.font.glyph_size;
}

static size_t pixel_stride(void) {
    size_t stride = (size_t)term_screen.stride / sizeof(pixel_t);
    if (!stride || stride < term_screen.width) {
        return term_screen.width;
    }

    return stride;
}

static void draw_glyph(size_t px, size_t py, u32 codepoint, u32 fg, u32 bg) {
    if (
        !term_screen.pixels ||
        !term_screen.width ||
        !term_screen.height ||
        px >= term_screen.width ||
        py >= term_screen.height
    ) {
        return;
    }

    const u8 *glyph = glyph_for(codepoint);
    size_t stride = pixel_stride();
    u32 *dst = term_screen.pixels + py * stride + px;

    u32 glyph_w = term_screen.font.width;
    u32 glyph_h = term_screen.font.height;
    u32 glyph_x0 = term_screen.cell_src_x;

    if (glyph_x0 >= glyph_w) {
        glyph_x0 = 0;
    }

    u32 src_w = glyph_w - glyph_x0;
    u32 cell_w = term_screen.cell_width;
    u32 cell_h = term_screen.cell_height;
    u32 draw_w = cell_w;
    u32 draw_h = cell_h;

    u32 right = term_screen.width - (u32)px;
    u32 bottom = term_screen.height - (u32)py;

    if (cell_w > right) {
        cell_w = right;
    }

    if (cell_h > bottom) {
        cell_h = bottom;
    }

    if (!cell_w || !cell_h) {
        return;
    }

    // Always clear the full cell so sub-glyph updates do not leave stale pixels.
    for (u32 by = 0; by < cell_h; by++) {
        u32 *row = dst + (size_t)by * stride;
        for (u32 bx = 0; bx < cell_w; bx++) {
            row[bx] = bg;
        }
    }

    if (draw_w > src_w) {
        draw_w = src_w;
    }

    if (draw_h > glyph_h) {
        draw_h = glyph_h;
    }

    if (draw_w > cell_w) {
        draw_w = cell_w;
    }

    if (draw_h > cell_h) {
        draw_h = cell_h;
    }

    if (!draw_w || !draw_h) {
        return;
    }

    if (glyph_x0 == 0 && draw_w == glyph_w && draw_h == glyph_h) {
        term_glyph_blit_u32(
            dst,
            stride,
            glyph,
            glyph_w,
            glyph_h,
            term_screen.font.row_bytes,
            fg,
            bg
        );
        return;
    }

    for (u32 gy = 0; gy < draw_h; gy++) {
        u32 *row = dst + (size_t)gy * stride;

        for (u32 gx = 0; gx < draw_w; gx++) {
            row[gx] = glyph_pixel_on(
                          glyph, term_screen.font.row_bytes, glyph_x0 + gx, gy
                      ) ?
                          fg :
                          bg;
        }
    }
}

static void draw_scrollbar(void) {
    if (!term_screen.scrollbar_width || !term_screen.width || !term_screen.height) {
        return;
    }

    framebuffer_t fb = _screen_framebuffer();
    i32 track_x = (i32)(term_screen.width - term_screen.scrollbar_width);
    draw_rect(
        &fb,
        track_x,
        0,
        term_screen.scrollbar_width,
        term_screen.height,
        TERM_SCROLLBAR_TRACK_COLOR
    );

    size_t total_lines = term_screen.history_count + term_screen.rows;
    if (total_lines <= term_screen.rows) {
        return;
    }

    u32 track_h = term_screen.height;
    u32 thumb_h = (u32)(((u64)term_screen.rows * (u64)track_h) / (u64)total_lines);
    if (thumb_h < TERM_SCROLLBAR_MIN_THUMB_PX) {
        thumb_h = TERM_SCROLLBAR_MIN_THUMB_PX;
    }
    if (thumb_h > track_h) {
        thumb_h = track_h;
    }

    u32 range = track_h - thumb_h;
    size_t max_offset = max_scroll_offset();
    size_t offset = clamped_scroll_offset();
    size_t pos_from_top = max_offset > offset ? (max_offset - offset) : 0;
    u32 thumb_y = 0;
    if (max_offset && range) {
        thumb_y = (u32)(((u64)pos_from_top * (u64)range) / (u64)max_offset);
    }

    pixel_t thumb_color =
        offset ? TERM_SCROLLBAR_THUMB_ACTIVE : TERM_SCROLLBAR_THUMB_COLOR;
    draw_rect(
        &fb,
        track_x,
        (i32)thumb_y,
        term_screen.scrollbar_width,
        thumb_h,
        thumb_color
    );
}

static bool term_screen_layout(
    u32 width,
    u32 height,
    size_t pixels_count,
    u32 *scrollbar_width_out,
    size_t *cols_out,
    size_t *rows_out
) {
    if (
        !width || !height || !pixels_count || !scrollbar_width_out || !cols_out ||
        !rows_out
    ) {
        return false;
    }

    size_t pixel_total = (size_t)width * (size_t)height;
    if (height && pixel_total / height != width) {
        return false;
    }

    if (pixel_total > pixels_count) {
        return false;
    }

    u32 scrollbar_width = 0;
    if (width > (term_screen.cell_width + TERM_SCROLLBAR_WIDTH_PX)) {
        scrollbar_width = TERM_SCROLLBAR_WIDTH_PX;
    }

    u32 text_width = width - scrollbar_width;
    size_t cols = text_width / term_screen.cell_width;
    size_t rows = height / term_screen.cell_height;

    if (!cols) {
        cols = 1;
        scrollbar_width = 0;
    }

    if (cols > TERM_MAX_COLS) {
        cols = TERM_MAX_COLS;
    }

    if (rows > TERM_MAX_ROWS) {
        rows = TERM_MAX_ROWS;
    }

    if (!rows) {
        rows = 1;
    }

    *scrollbar_width_out = scrollbar_width;
    *cols_out = cols;
    *rows_out = rows;
    return true;
}

bool term_screen_init(const framebuffer_t *fb) {
    if (!fb || !fb->pixels || !fb->width || !fb->height || !fb->pixel_count) {
        return false;
    }

    if (!init_font()) {
        return false;
    }

    size_t cols = 0;
    size_t rows = 0;
    u32 scrollbar_width = 0;
    bool layout_ok = term_screen_layout(
        fb->width,
        fb->height,
        fb->pixel_count,
        &scrollbar_width,
        &cols,
        &rows
    );

    if (!layout_ok) {
        return false;
    }

    term_screen.width = fb->width;
    term_screen.height = fb->height;
    term_screen.stride = fb->stride;
    term_screen.pixels = fb->pixels;
    term_screen.pixels_count = fb->pixel_count;
    term_screen.scrollbar_width = scrollbar_width;
    term_screen.cols = cols;
    term_screen.rows = rows;
    clear_history();
    term_screen.cursor_x = 0;
    term_screen.cursor_y = 0;
    term_screen.saved_x = 0;
    term_screen.saved_y = 0;
    term_screen.cursor_visible = true;
    term_screen.cursor_prev_valid = false;
    term_screen.scrollbar_dirty = true;

    ansi_color_reset(&term_screen.color);
    term_utf8_reset(&term_screen.utf8);
    ansi_parser_init(&term_screen.ansi);
    clear_cells();
    clear_pixels_full();
    mark_dirty_all();

    term_screen.ready = true;
    return true;
}

bool term_screen_resize(const framebuffer_t *fb) {
    if (!term_screen.ready || !fb || !fb->pixels) {
        return false;
    }

    size_t cols = 0;
    size_t rows = 0;
    u32 scrollbar_width = 0;
    bool layout_ok = term_screen_layout(
        fb->width,
        fb->height,
        fb->pixel_count,
        &scrollbar_width,
        &cols,
        &rows
    );

    if (!layout_ok) {
        return false;
    }

    term_cell_t *old_cells = malloc(sizeof(term_screen.cells));
    if (!old_cells) {
        return false;
    }

    memcpy(old_cells, term_screen.cells, sizeof(term_screen.cells));

    size_t old_cols = term_screen.cols;
    size_t old_rows = term_screen.rows;

    term_screen.width = fb->width;
    term_screen.height = fb->height;
    term_screen.stride = fb->stride;
    term_screen.pixels = fb->pixels;
    term_screen.pixels_count = fb->pixel_count;
    term_screen.scrollbar_width = scrollbar_width;
    term_screen.cols = cols;
    term_screen.rows = rows;
    clear_history();

    clear_cells();

    size_t copy_rows = old_rows;
    if (rows < copy_rows) {
        copy_rows = rows;
    }

    size_t copy_cols = old_cols;
    if (cols < copy_cols) {
        copy_cols = cols;
    }

    for (size_t y = 0; y < copy_rows; y++) {
        for (size_t x = 0; x < copy_cols; x++) {
            term_screen.cells[y * cols + x] = old_cells[y * old_cols + x];
        }
    }

    free(old_cells);

    term_cursor_clamp(
        &term_screen.cursor_x,
        &term_screen.cursor_y,
        &term_screen.saved_x,
        &term_screen.saved_y,
        NULL,
        cols,
        rows
    );

    if (term_screen.cursor_prev_x >= cols || term_screen.cursor_prev_y >= rows) {
        term_screen.cursor_prev_valid = false;
    }

    term_screen.scrollbar_dirty = true;
    clear_pixels_full();
    mark_dirty_all();

    return true;
}

void term_screen_reset(void) {
    if (!term_screen.ready) {
        return;
    }

    term_screen.cursor_x = 0;
    term_screen.cursor_y = 0;
    term_screen.saved_x = 0;
    term_screen.saved_y = 0;
    term_screen.cursor_visible = true;
    term_screen.cursor_prev_valid = false;
    clear_history();
    term_screen.scrollbar_dirty = true;

    ansi_color_reset(&term_screen.color);
    term_utf8_reset(&term_screen.utf8);
    ansi_parser_reset(&term_screen.ansi);
    clear_cells();
    clear_pixels_full();
    mark_dirty_all();
}

void term_screen_feed(const u8 *bytes, size_t len) {
    if (!term_screen.ready || !bytes || !len) {
        return;
    }

    for (size_t i = 0; i < len; i++) {
        if (bytes[i] == '\x1b') {
            term_utf8_flush_invalid(
                &term_screen.utf8, &term_utf8_callbacks, NULL
            );
        }

        ansi_parser_feed(
            &term_screen.ansi, bytes[i], &term_ansi_callbacks, NULL
        );
    }
}

bool term_screen_render_rect(u32 *x, u32 *y, u32 *width, u32 *height) {
    if (!term_screen.ready) {
        return false;
    }

    if (!term_screen.dirty) {
        return false;
    }

    size_t x0 = term_screen.dirty_x0;
    size_t y0 = term_screen.dirty_y0;
    size_t x1 = term_screen.dirty_x1;
    size_t y1 = term_screen.dirty_y1;

    for (size_t row = y0; row < y1; row++) {
        const term_cell_t *line = view_row_cells(row);
        if (!line) {
            continue;
        }

        for (size_t col = x0; col < x1; col++) {
            size_t px = col * term_screen.cell_width;
            size_t py = row * term_screen.cell_height;

            const term_cell_t *cell = &line[col];
            u32 fg = ansi_color_rgb(cell->fg);
            u32 bg = ansi_color_rgb(cell->bg);

            draw_glyph(px, py, cell->codepoint, fg, bg);
        }
    }

    if (
        term_screen.scroll_offset == 0 &&
        term_screen.cursor_visible &&
        term_screen.cursor_x < term_screen.cols &&
        term_screen.cursor_y < term_screen.rows
    ) {
        size_t px = term_screen.cursor_x * term_screen.cell_width;
        size_t py = term_screen.cursor_y * term_screen.cell_height;

        term_cell_t *cell = cell_at(term_screen.cursor_x, term_screen.cursor_y);
        u32 fg = ansi_color_rgb(cell->fg);
        u32 bg = ansi_color_rgb(cell->bg);

        framebuffer_t fb = _screen_framebuffer();
        draw_rect(
            &fb,
            (i32)px,
            (i32)py,
            term_screen.cell_width,
            term_screen.cell_height,
            bg
        );
        draw_glyph(px, py, cell->codepoint, bg, fg);

        term_screen.cursor_prev_x = term_screen.cursor_x;
        term_screen.cursor_prev_y = term_screen.cursor_y;
        term_screen.cursor_prev_valid = true;
    } else {
        term_screen.cursor_prev_valid = false;
    }

    bool drew_scrollbar = false;
    if (term_screen.scrollbar_width && term_screen.scrollbar_dirty) {
        draw_scrollbar();
        drew_scrollbar = true;
        term_screen.scrollbar_dirty = false;
    }

    term_screen.dirty = false;

    u32 out_x = (u32)(x0 * term_screen.cell_width);
    u32 out_y = (u32)(y0 * term_screen.cell_height);
    u32 out_width = (u32)((x1 - x0) * term_screen.cell_width);
    u32 out_height = (u32)((y1 - y0) * term_screen.cell_height);

    if (x0 == 0 && x1 == term_screen.cols) {
        out_width = term_screen.width;
    }

    if (y0 == 0 && y1 == term_screen.rows) {
        out_height = term_screen.height;
    }

    if (drew_scrollbar) {
        u32 bar_x = term_screen.width - term_screen.scrollbar_width;
        u32 bar_y = 0;
        u32 bar_w = term_screen.scrollbar_width;
        u32 bar_h = term_screen.height;

        u32 out_x1 = out_x + out_width;
        u32 out_y1 = out_y + out_height;
        u32 bar_x1 = bar_x + bar_w;
        u32 bar_y1 = bar_y + bar_h;

        if (bar_x < out_x) {
            out_x = bar_x;
        }
        if (bar_y < out_y) {
            out_y = bar_y;
        }

        if (bar_x1 > out_x1) {
            out_x1 = bar_x1;
        }
        if (bar_y1 > out_y1) {
            out_y1 = bar_y1;
        }

        out_width = out_x1 - out_x;
        out_height = out_y1 - out_y;
    }

    if (x) {
        *x = out_x;
    }

    if (y) {
        *y = out_y;
    }

    if (width) {
        *width = out_width;
    }

    if (height) {
        *height = out_height;
    }

    return true;
}

size_t term_screen_cols(void) {
    return term_screen.cols;
}

size_t term_screen_rows(void) {
    return term_screen.rows;
}

bool term_screen_scroll_lines(int lines) {
    if (!term_screen.ready || !lines) {
        return false;
    }

    size_t old_offset = clamped_scroll_offset();
    size_t max = max_scroll_offset();
    size_t next = old_offset;

    if (lines > 0) {
        size_t delta = (size_t)lines;
        if (delta > max - next) {
            next = max;
        } else {
            next += delta;
        }
    } else {
        size_t delta = (size_t)(-lines);
        if (delta > next) {
            next = 0;
        } else {
            next -= delta;
        }
    }

    if (next == old_offset) {
        return false;
    }

    term_screen.scroll_offset = next;
    term_screen.scrollbar_dirty = true;
    mark_dirty_all();
    return true;
}

void term_screen_scroll_bottom(void) {
    if (!term_screen.ready || !term_screen.scroll_offset) {
        return;
    }

    term_screen.scroll_offset = 0;
    term_screen.scrollbar_dirty = true;
    mark_dirty_all();
}

size_t term_screen_scrollback_lines(void) {
    return term_screen.history_count;
}

size_t term_screen_scroll_offset(void) {
    return clamped_scroll_offset();
}
