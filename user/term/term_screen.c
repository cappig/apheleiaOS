#include "term_screen.h"

#include <draw.h>
#include <fcntl.h>
#include <psf.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <term/ansi.h>
#include <unistd.h>

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
    u8 font_buf[FONT_BUF_SIZE];
    u32* pixels;
    size_t pixels_count;
    u32 width;
    u32 height;
    size_t cols;
    size_t rows;
    size_t cursor_x;
    size_t cursor_y;
    size_t saved_x;
    size_t saved_y;
    bool cursor_visible;
    ansi_parser_t ansi;
    bool ready;
} term_screen_state_t;

static term_screen_state_t term_screen = {0};

static bool load_font_file(const char* path) {
    return psf_load_file(path, term_screen.font_buf, sizeof(term_screen.font_buf), &term_screen.font);
}

static bool init_font(void) {
    if (term_screen.font.glyphs)
        return true;

    if (load_font_file("/boot/font.psf"))
        return true;

    if (load_font_file("/etc/font.psf"))
        return true;

    return false;
}

static void clear_cells(void) {
    for (size_t y = 0; y < term_screen.rows; y++) {
        for (size_t x = 0; x < term_screen.cols; x++)
            term_screen.cells[y][x].ch = ' ';
    }
}

static void scroll_up(void) {
    for (size_t y = 1; y < term_screen.rows; y++) {
        for (size_t x = 0; x < term_screen.cols; x++)
            term_screen.cells[y - 1][x] = term_screen.cells[y][x];
    }

    for (size_t x = 0; x < term_screen.cols; x++)
        term_screen.cells[term_screen.rows - 1][x].ch = ' ';
}

static void newline(void) {
    term_screen.cursor_x = 0;
    term_screen.cursor_y++;

    if (term_screen.cursor_y < term_screen.rows)
        return;

    term_screen.cursor_y = term_screen.rows - 1;
    scroll_up();
}

static size_t next_tab_stop(size_t cursor_x) {
    return ((cursor_x / TERM_TAB_WIDTH) + 1) * TERM_TAB_WIDTH;
}

static void put_char(char ch) {
    if (ch == '\r') {
        term_screen.cursor_x = 0;
        return;
    }

    if (ch == '\n') {
        newline();
        return;
    }

    if (ch == '\b') {
        if (term_screen.cursor_x > 0)
            term_screen.cursor_x--;
        return;
    }

    if (ch == '\t') {
        size_t next = next_tab_stop(term_screen.cursor_x);

        while (term_screen.cursor_x < next)
            put_char(' ');

        return;
    }

    if ((unsigned char)ch < 32)
        return;

    if (term_screen.cursor_x >= term_screen.cols)
        newline();

    term_screen.cells[term_screen.cursor_y][term_screen.cursor_x].ch = ch;
    term_screen.cursor_x++;

    if (term_screen.cursor_x >= term_screen.cols)
        newline();
}

static void clear_line_mode(int mode) {
    if (mode == 2) {
        for (size_t x = 0; x < term_screen.cols; x++)
            term_screen.cells[term_screen.cursor_y][x].ch = ' ';

        return;
    }

    if (mode == 1) {
        for (size_t x = 0; x <= term_screen.cursor_x && x < term_screen.cols; x++)
            term_screen.cells[term_screen.cursor_y][x].ch = ' ';

        return;
    }

    for (size_t x = term_screen.cursor_x; x < term_screen.cols; x++)
        term_screen.cells[term_screen.cursor_y][x].ch = ' ';
}

static void clear_screen_mode(int mode) {
    if (mode == 2) {
        clear_cells();
        term_screen.cursor_x = 0;
        term_screen.cursor_y = 0;
        return;
    }

    if (mode == 1) {
        for (size_t y = 0; y <= term_screen.cursor_y && y < term_screen.rows; y++) {
            size_t end = (y == term_screen.cursor_y) ? term_screen.cursor_x : (term_screen.cols - 1);

            for (size_t x = 0; x <= end && x < term_screen.cols; x++)
                term_screen.cells[y][x].ch = ' ';
        }
        return;
    }

    for (size_t y = term_screen.cursor_y; y < term_screen.rows; y++) {
        size_t start = (y == term_screen.cursor_y) ? term_screen.cursor_x : 0;

        for (size_t x = start; x < term_screen.cols; x++)
            term_screen.cells[y][x].ch = ' ';
    }
}

static void ansi_on_print(void* ctx, u8 ch) {
    (void)ctx;
    put_char((char)ch);
}

static void ansi_on_control(void* ctx, u8 ch) {
    (void)ctx;
    put_char((char)ch);
}

static void ansi_on_csi(void* ctx, char op, const int* params, size_t count, bool private_mode) {
    (void)ctx;

    int n = ansi_param(params, count, 0, 1);
    int row = 1;
    int col = 1;
    if (n < 1)
        n = 1;

    switch (op) {
    case 'A':
        if ((size_t)n > term_screen.cursor_y)
            term_screen.cursor_y = 0;
        else
            term_screen.cursor_y -= (size_t)n;

        break;
    case 'B':
        term_screen.cursor_y += (size_t)n;

        if (term_screen.cursor_y >= term_screen.rows)
            term_screen.cursor_y = term_screen.rows - 1;

        break;
    case 'C':
        term_screen.cursor_x += (size_t)n;

        if (term_screen.cursor_x >= term_screen.cols)
            term_screen.cursor_x = term_screen.cols - 1;

        break;
    case 'D':
        if ((size_t)n > term_screen.cursor_x)
            term_screen.cursor_x = 0;
        else
            term_screen.cursor_x -= (size_t)n;

        break;
    case 'G':
        col = ansi_param(params, count, 0, 1);

        if (col < 1)
            col = 1;

        term_screen.cursor_x = (size_t)(col - 1);

        if (term_screen.cursor_x >= term_screen.cols)
            term_screen.cursor_x = term_screen.cols - 1;

        break;
    case 'H':
    case 'f':
        row = ansi_param(params, count, 0, 1);
        col = ansi_param(params, count, 1, 1);

        if (row < 1)
            row = 1;

        if (col < 1)
            col = 1;

        term_screen.cursor_y = (size_t)(row - 1);
        term_screen.cursor_x = (size_t)(col - 1);

        if (term_screen.cursor_y >= term_screen.rows)
            term_screen.cursor_y = term_screen.rows - 1;

        if (term_screen.cursor_x >= term_screen.cols)
            term_screen.cursor_x = term_screen.cols - 1;

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
        term_screen.cursor_x = term_screen.saved_x < term_screen.cols ? term_screen.saved_x : term_screen.cols - 1;
        term_screen.cursor_y = term_screen.saved_y < term_screen.rows ? term_screen.saved_y : term_screen.rows - 1;
        break;
    case 'h':
        if (private_mode && ansi_param(params, count, 0, 0) == 25)
            term_screen.cursor_visible = true;
        break;
    case 'l':
        if (private_mode && ansi_param(params, count, 0, 0) == 25)
            term_screen.cursor_visible = false;
        break;
    case 'm':
        break;
    default:
        break;
    }
}

static const ansi_callbacks_t term_ansi_callbacks = {
    .on_print = ansi_on_print,
    .on_control = ansi_on_control,
    .on_csi = ansi_on_csi,
    .on_escape = NULL,
};

static const u8* glyph_for(char ch) {
    u32 idx = (u8)ch;
    if (idx >= term_screen.font.glyph_count)
        idx = '?';

    return term_screen.font.glyphs + (size_t)idx * term_screen.font.glyph_size;
}

static void draw_glyph(size_t px, size_t py, char ch, u32 fg, u32 bg) {
    const u8* glyph = glyph_for(ch);

    for (u32 gy = 0; gy < term_screen.font.height; gy++) {
        const u8* row_ptr = glyph + gy * term_screen.font.row_bytes;
        u32* row = term_screen.pixels + (py + gy) * term_screen.width + px;

        for (u32 gx = 0; gx < term_screen.font.width; gx++) {
            u8 bits = row_ptr[gx / 8];
            u8 mask = (u8)(0x80 >> (gx & 7));
            row[gx] = (bits & mask) ? fg : bg;
        }
    }
}

bool term_screen_init(u32 width, u32 height, u32* pixels, size_t pixels_count) {
    if (!pixels || !width || !height || !pixels_count)
        return false;

    if (!init_font())
        return false;

    if (width > TERM_MAX_W || height > TERM_MAX_H)
        return false;

    size_t cols = width / term_screen.font.width;
    size_t rows = height / term_screen.font.height;

    if (!cols || !rows || cols > TERM_MAX_COLS || rows > TERM_MAX_ROWS)
        return false;

    term_screen.width = width;
    term_screen.height = height;
    term_screen.pixels = pixels;
    term_screen.pixels_count = pixels_count;
    term_screen.cols = cols;
    term_screen.rows = rows;
    term_screen.cursor_x = 0;
    term_screen.cursor_y = 0;
    term_screen.saved_x = 0;
    term_screen.saved_y = 0;
    term_screen.cursor_visible = true;

    ansi_parser_init(&term_screen.ansi);
    clear_cells();

    term_screen.ready = true;
    return true;
}

void term_screen_reset(void) {
    if (!term_screen.ready)
        return;

    term_screen.cursor_x = 0;
    term_screen.cursor_y = 0;
    term_screen.saved_x = 0;
    term_screen.saved_y = 0;
    term_screen.cursor_visible = true;

    ansi_parser_reset(&term_screen.ansi);
    clear_cells();
}

void term_screen_feed(const u8* bytes, size_t len) {
    if (!term_screen.ready || !bytes || !len)
        return;

    for (size_t i = 0; i < len; i++)
        ansi_parser_feed(&term_screen.ansi, bytes[i], &term_ansi_callbacks, NULL);
}

void term_screen_render(void) {
    if (!term_screen.ready)
        return;

    draw_fill_rect(term_screen.pixels, term_screen.width, term_screen.height, 0, 0, term_screen.width, term_screen.height, TERM_BG);

    for (size_t y = 0; y < term_screen.rows; y++) {
        for (size_t x = 0; x < term_screen.cols; x++) {
            size_t px = x * term_screen.font.width;
            size_t py = y * term_screen.font.height;
            draw_glyph(px, py, term_screen.cells[y][x].ch, TERM_FG, TERM_BG);
        }
    }

    if (term_screen.cursor_visible && term_screen.cursor_x < term_screen.cols && term_screen.cursor_y < term_screen.rows) {
        size_t px = term_screen.cursor_x * term_screen.font.width;
        size_t py = term_screen.cursor_y * term_screen.font.height;

        draw_fill_rect(term_screen.pixels, term_screen.width, term_screen.height, (i32)px, (i32)py, term_screen.font.width, term_screen.font.height, TERM_FG);
        draw_glyph(px, py, term_screen.cells[term_screen.cursor_y][term_screen.cursor_x].ch, TERM_BG, TERM_FG);
    }
}

size_t term_screen_cols(void) {
    return term_screen.cols;
}

size_t term_screen_rows(void) {
    return term_screen.rows;
}
