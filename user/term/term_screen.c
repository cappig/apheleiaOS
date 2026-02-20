#include "term_screen.h"

#include <draw.h>
#include <psf.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <term/ansi.h>

#define TERM_BG 0x001a1a1aU
#define TERM_FG 0x00f0f0f0U

#define FONT_BUF_SIZE  (256 * 1024)
#define TERM_TAB_WIDTH 8

typedef struct {
    char ch;
} term_cell_t;

typedef struct {
    psf_font_t font;
    term_cell_t cells[TERM_MAX_ROWS][TERM_MAX_COLS];
    u8 *font_buf;
    pixel_t *pixels;
    size_t pixels_count;
    u32 width;
    u32 height;
    u32 stride;
    size_t cols;
    size_t rows;
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
    ansi_parser_t ansi;
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

static void clear_pixels_full(void) {
    if (!term_screen.pixels || !term_screen.width || !term_screen.height) {
        return;
    }

    framebuffer_t fb = _screen_framebuffer();
    draw_rect(&fb, 0, 0, term_screen.width, term_screen.height, TERM_BG);
}

static bool load_font_file(const char *path) {
    return psf_load_file(path, term_screen.font_buf, FONT_BUF_SIZE, &term_screen.font);
}

static bool init_font(void) {
    if (term_screen.font.glyphs) {
        return true;
    }

    if (!term_screen.font_buf) {
        term_screen.font_buf = malloc(FONT_BUF_SIZE);
        if (!term_screen.font_buf) {
            return false;
        }
    }

    if (load_font_file("/boot/font.psf")) {
        return true;
    }

    if (load_font_file("/etc/font.psf")) {
        return true;
    }

    return false;
}

static void mark_dirty_rect(size_t x0, size_t y0, size_t x1, size_t y1) {
    if (!term_screen.cols || !term_screen.rows || x0 >= x1 || y0 >= y1) {
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
    mark_dirty_rect(0, 0, term_screen.cols, term_screen.rows);
}

static void mark_cursor_move(size_t old_x, size_t old_y) {
    if (old_x < term_screen.cols && old_y < term_screen.rows) {
        mark_dirty_cell(old_x, old_y);
    }

    if (term_screen.cursor_x < term_screen.cols && term_screen.cursor_y < term_screen.rows) {
        mark_dirty_cell(term_screen.cursor_x, term_screen.cursor_y);
    }
}

static void clear_cells(void) {
    for (size_t y = 0; y < term_screen.rows; y++) {
        for (size_t x = 0; x < term_screen.cols; x++) {
            term_screen.cells[y][x].ch = ' ';
        }
    }
}

static void scroll_up(void) {
    for (size_t y = 1; y < term_screen.rows; y++) {
        for (size_t x = 0; x < term_screen.cols; x++) {
            term_screen.cells[y - 1][x] = term_screen.cells[y][x];
        }
    }

    for (size_t x = 0; x < term_screen.cols; x++) {
        term_screen.cells[term_screen.rows - 1][x].ch = ' ';
    }

    mark_dirty_all();
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

static void put_char(char ch) {
    size_t old_x = term_screen.cursor_x;
    size_t old_y = term_screen.cursor_y;

    if (ch == '\r') {
        term_screen.cursor_x = 0;
        mark_cursor_move(old_x, old_y);
        return;
    }

    if (ch == '\n') {
        newline();
        mark_cursor_move(old_x, old_y);
        return;
    }

    if (ch == '\b') {
        if (term_screen.cursor_x > 0) {
            term_screen.cursor_x--;
        }
        mark_cursor_move(old_x, old_y);
        return;
    }

    if (ch == '\t') {
        size_t next = next_tab_stop(term_screen.cursor_x);

        while (term_screen.cursor_x < next) {
            put_char(' ');
        }

        return;
    }

    if ((unsigned char)ch < 32) {
        return;
    }

    if (term_screen.cursor_x >= term_screen.cols) {
        newline();
    }

    term_cell_t *cell = &term_screen.cells[term_screen.cursor_y][term_screen.cursor_x];
    if (cell->ch != ch) {
        cell->ch = ch;
        mark_dirty_cell(term_screen.cursor_x, term_screen.cursor_y);
    }

    term_screen.cursor_x++;

    mark_cursor_move(old_x, old_y);
}

static void clear_line_mode(int mode) {
    size_t y = term_screen.cursor_y;

    if (mode == 2) {
        for (size_t x = 0; x < term_screen.cols; x++) {
            term_screen.cells[y][x].ch = ' ';
        }

        mark_dirty_rect(0, y, term_screen.cols, y + 1);
        return;
    }

    if (mode == 1) {
        size_t end = term_screen.cursor_x + 1;
        if (end > term_screen.cols) {
            end = term_screen.cols;
        }

        for (size_t x = 0; x < end; x++) {
            term_screen.cells[y][x].ch = ' ';
        }

        mark_dirty_rect(0, y, end, y + 1);
        return;
    }

    for (size_t x = term_screen.cursor_x; x < term_screen.cols; x++) {
        term_screen.cells[y][x].ch = ' ';
    }

    mark_dirty_rect(term_screen.cursor_x, y, term_screen.cols, y + 1);
}

static void clear_screen_mode(int mode) {
    if (mode == 2) {
        clear_cells();
        term_screen.cursor_x = 0;
        term_screen.cursor_y = 0;
        mark_dirty_all();
        return;
    }

    if (mode == 1) {
        for (size_t y = 0; y <= term_screen.cursor_y && y < term_screen.rows; y++) {
            size_t end = y == term_screen.cursor_y ? term_screen.cursor_x + 1 : term_screen.cols;
            if (end > term_screen.cols) {
                end = term_screen.cols;
            }

            for (size_t x = 0; x < end; x++) {
                term_screen.cells[y][x].ch = ' ';
            }
        }

        mark_dirty_rect(0, 0, term_screen.cols, term_screen.cursor_y + 1);
        return;
    }

    for (size_t y = term_screen.cursor_y; y < term_screen.rows; y++) {
        size_t start = y == term_screen.cursor_y ? term_screen.cursor_x : 0;
        for (size_t x = start; x < term_screen.cols; x++) {
            term_screen.cells[y][x].ch = ' ';
        }
    }

    mark_dirty_rect(0, term_screen.cursor_y, term_screen.cols, term_screen.rows);
}

static void ansi_on_print(void *ctx, u8 ch) {
    (void)ctx;
    put_char((char)ch);
}

static void ansi_on_control(void *ctx, u8 ch) {
    (void)ctx;
    put_char((char)ch);
}

static void ansi_on_csi(void *ctx, char op, const int *params, size_t count, bool private_mode) {
    (void)ctx;

    size_t old_x = term_screen.cursor_x;
    size_t old_y = term_screen.cursor_y;
    int n = ansi_param(params, count, 0, 1);
    int row = 1;
    int col = 1;

    if (n < 1) {
        n = 1;
    }

    switch (op) {
    case 'A':
        if ((size_t)n > term_screen.cursor_y) {
            term_screen.cursor_y = 0;
        } else {
            term_screen.cursor_y -= (size_t)n;
        }
        break;
    case 'B':
        term_screen.cursor_y += (size_t)n;
        if (term_screen.cursor_y >= term_screen.rows) {
            term_screen.cursor_y = term_screen.rows - 1;
        }
        break;
    case 'C':
        term_screen.cursor_x += (size_t)n;
        if (term_screen.cursor_x >= term_screen.cols) {
            term_screen.cursor_x = term_screen.cols - 1;
        }
        break;
    case 'D':
        if ((size_t)n > term_screen.cursor_x) {
            term_screen.cursor_x = 0;
        } else {
            term_screen.cursor_x -= (size_t)n;
        }
        break;
    case 'G':
        col = ansi_param(params, count, 0, 1);
        if (col < 1) {
            col = 1;
        }

        term_screen.cursor_x = (size_t)(col - 1);
        if (term_screen.cursor_x >= term_screen.cols) {
            term_screen.cursor_x = term_screen.cols - 1;
        }
        break;
    case 'H':
    case 'f':
        row = ansi_param(params, count, 0, 1);
        col = ansi_param(params, count, 1, 1);

        if (row < 1) {
            row = 1;
        }

        if (col < 1) {
            col = 1;
        }

        term_screen.cursor_y = (size_t)(row - 1);
        term_screen.cursor_x = (size_t)(col - 1);

        if (term_screen.cursor_y >= term_screen.rows) {
            term_screen.cursor_y = term_screen.rows - 1;
        }

        if (term_screen.cursor_x >= term_screen.cols) {
            term_screen.cursor_x = term_screen.cols - 1;
        }
        break;
    case 'J':
        clear_screen_mode(ansi_param(params, count, 0, 0));
        break;
    case 'K':
        clear_line_mode(ansi_param(params, count, 0, 0));
        break;
    case 's':
        term_screen.saved_x = term_screen.cursor_x;
        term_screen.saved_y = term_screen.cursor_y;
        break;
    case 'u':
        term_screen.cursor_x =
            term_screen.saved_x < term_screen.cols ? term_screen.saved_x : term_screen.cols - 1;
        term_screen.cursor_y =
            term_screen.saved_y < term_screen.rows ? term_screen.saved_y : term_screen.rows - 1;
        break;
    case 'h':
        if (private_mode && ansi_param(params, count, 0, 0) == 25) {
            term_screen.cursor_visible = true;
        }
        break;
    case 'l':
        if (private_mode && ansi_param(params, count, 0, 0) == 25) {
            term_screen.cursor_visible = false;
        }
        break;
    case 'm':
        break;
    default:
        break;
    }

    mark_cursor_move(old_x, old_y);
}

static const ansi_callbacks_t term_ansi_callbacks = {
    .on_print = ansi_on_print,
    .on_control = ansi_on_control,
    .on_csi = ansi_on_csi,
    .on_escape = NULL,
};

static const u8 *glyph_for(char ch) {
    u32 idx = (u8)ch;
    if (idx >= term_screen.font.glyph_count) {
        idx = '?';
    }

    return term_screen.font.glyphs + (size_t)idx * term_screen.font.glyph_size;
}

static void draw_glyph(size_t px, size_t py, char ch, u32 fg, u32 bg) {
    const u8 *glyph = glyph_for(ch);

    for (u32 gy = 0; gy < term_screen.font.height; gy++) {
        const u8 *row_ptr = glyph + gy * term_screen.font.row_bytes;
        u32 *row = term_screen.pixels + (py + gy) * term_screen.width + px;

        for (u32 gx = 0; gx < term_screen.font.width; gx++) {
            u8 bits = row_ptr[gx / 8];
            u8 mask = (u8)(0x80 >> (gx & 7));
            row[gx] = (bits & mask) ? fg : bg;
        }
    }
}

static bool term_screen_layout(
    u32 width,
    u32 height,
    size_t pixels_count,
    size_t *cols_out,
    size_t *rows_out
) {
    if (!width || !height || !pixels_count || !cols_out || !rows_out) {
        return false;
    }

    size_t pixel_total = (size_t)width * (size_t)height;
    if (height && pixel_total / height != width) {
        return false;
    }

    if (pixel_total > pixels_count) {
        return false;
    }

    size_t cols = width / term_screen.font.width;
    size_t rows = height / term_screen.font.height;

    if (cols > TERM_MAX_COLS) {
        cols = TERM_MAX_COLS;
    }

    if (rows > TERM_MAX_ROWS) {
        rows = TERM_MAX_ROWS;
    }

    if (!cols || !rows) {
        return false;
    }

    *cols_out = cols;
    *rows_out = rows;
    return true;
}

bool term_screen_can_resize(u32 width, u32 height) {
    if (!width || !height) {
        return false;
    }

    if (!init_font()) {
        return false;
    }

    size_t cols = width / term_screen.font.width;
    size_t rows = height / term_screen.font.height;
    if (!cols || !rows || cols > TERM_MAX_COLS || rows > TERM_MAX_ROWS) {
        return false;
    }

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
    if (!term_screen_layout(fb->width, fb->height, fb->pixel_count, &cols, &rows)) {
        return false;
    }

    term_screen.width = fb->width;
    term_screen.height = fb->height;
    term_screen.stride = fb->stride;
    term_screen.pixels = fb->pixels;
    term_screen.pixels_count = fb->pixel_count;
    term_screen.cols = cols;
    term_screen.rows = rows;
    term_screen.cursor_x = 0;
    term_screen.cursor_y = 0;
    term_screen.saved_x = 0;
    term_screen.saved_y = 0;
    term_screen.cursor_visible = true;
    term_screen.cursor_prev_valid = false;

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
    if (!term_screen_layout(fb->width, fb->height, fb->pixel_count, &cols, &rows)) {
        return false;
    }

    term_cell_t old_cells[TERM_MAX_ROWS][TERM_MAX_COLS];
    memcpy(old_cells, term_screen.cells, sizeof(old_cells));

    size_t old_cols = term_screen.cols;
    size_t old_rows = term_screen.rows;

    term_screen.width = fb->width;
    term_screen.height = fb->height;
    term_screen.stride = fb->stride;
    term_screen.pixels = fb->pixels;
    term_screen.pixels_count = fb->pixel_count;
    term_screen.cols = cols;
    term_screen.rows = rows;

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
            term_screen.cells[y][x] = old_cells[y][x];
        }
    }

    if (term_screen.cursor_x >= cols) {
        term_screen.cursor_x = cols - 1;
    }
    if (term_screen.cursor_y >= rows) {
        term_screen.cursor_y = rows - 1;
    }

    if (term_screen.saved_x >= cols) {
        term_screen.saved_x = cols - 1;
    }
    if (term_screen.saved_y >= rows) {
        term_screen.saved_y = rows - 1;
    }

    if (term_screen.cursor_prev_x >= cols || term_screen.cursor_prev_y >= rows) {
        term_screen.cursor_prev_valid = false;
    }

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
        ansi_parser_feed(&term_screen.ansi, bytes[i], &term_ansi_callbacks, NULL);
    }
}

bool term_screen_render_rect(u32 *x, u32 *y, u32 *width, u32 *height) {
    if (!term_screen.ready) {
        return false;
    }

    if (term_screen.cursor_prev_valid) {
        mark_dirty_cell(term_screen.cursor_prev_x, term_screen.cursor_prev_y);
    }

    if (term_screen.cursor_visible && term_screen.cursor_x < term_screen.cols &&
        term_screen.cursor_y < term_screen.rows) {
        mark_dirty_cell(term_screen.cursor_x, term_screen.cursor_y);
    }

    if (!term_screen.dirty) {
        return false;
    }

    size_t x0 = term_screen.dirty_x0;
    size_t y0 = term_screen.dirty_y0;
    size_t x1 = term_screen.dirty_x1;
    size_t y1 = term_screen.dirty_y1;

    for (size_t row = y0; row < y1; row++) {
        for (size_t col = x0; col < x1; col++) {
            size_t px = col * term_screen.font.width;
            size_t py = row * term_screen.font.height;
            draw_glyph(px, py, term_screen.cells[row][col].ch, TERM_FG, TERM_BG);
        }
    }

    if (term_screen.cursor_visible && term_screen.cursor_x < term_screen.cols &&
        term_screen.cursor_y < term_screen.rows) {
        size_t px = term_screen.cursor_x * term_screen.font.width;
        size_t py = term_screen.cursor_y * term_screen.font.height;

        framebuffer_t fb = _screen_framebuffer();
        draw_rect(&fb, (i32)px, (i32)py, term_screen.font.width, term_screen.font.height, TERM_FG);
        draw_glyph(
            px,
            py,
            term_screen.cells[term_screen.cursor_y][term_screen.cursor_x].ch,
            TERM_BG,
            TERM_FG
        );
        term_screen.cursor_prev_x = term_screen.cursor_x;
        term_screen.cursor_prev_y = term_screen.cursor_y;
        term_screen.cursor_prev_valid = true;
    } else {
        term_screen.cursor_prev_valid = false;
    }

    term_screen.dirty = false;

    u32 out_x = (u32)(x0 * term_screen.font.width);
    u32 out_y = (u32)(y0 * term_screen.font.height);
    u32 out_width = (u32)((x1 - x0) * term_screen.font.width);
    u32 out_height = (u32)((y1 - y0) * term_screen.font.height);

    // If dirty spans full cell-space extents, include any partial right/bottom
    // pixel strips so expanded windows do not retain stale/black edges.
    if (x0 == 0 && x1 == term_screen.cols) {
        out_width = term_screen.width;
    }

    if (y0 == 0 && y1 == term_screen.rows) {
        out_height = term_screen.height;
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
