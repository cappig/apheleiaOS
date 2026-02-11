#include "console.h"

#include <arch/arch.h>
#include <base/macros.h>
#include <base/types.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/font.h>
#include <sys/tty.h>
#include <x86/serial.h>
#include <x86/vga.h>

typedef enum {
    CONSOLE_DISABLED = 0,
    CONSOLE_VGA = 1,
    CONSOLE_VESA = 2,
} console_mode_t;

typedef struct {
    size_t cursor_x;
    size_t cursor_y;
    u8 vga_attr;
    u8 fg_vga;
    u8 bg_vga;
    u32 fg_rgb;
    u32 bg_rgb;
    bool bright;
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
} console_state_t;

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

static void _init_screens(size_t active_screen);
static void _free_screens(void);
static void _redraw_screen(size_t index);
static void _write_screen(size_t screen_index, const char* buf, size_t len);
static bool _set_active(size_t index);
static void _clear_vga(const console_screen_t* screen);
static void _clear_vesa(const console_screen_t* screen);
static void _draw_char_vesa(u32 codepoint, size_t col, size_t row, u32 fg_rgb, u32 bg_rgb);
static void _draw_char_vga(u32 codepoint, size_t col, size_t row, u8 attr);

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

    _screen_reset_colors(screen);
    screen->cursor_x = 0;
    screen->cursor_y = 0;
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

static void _redraw_screen(size_t index) {
    console_screen_t* screen = _get_screen(index);
    console_cell_t* cells = _screen_cells(index);

    if (!screen || !console_state.ready)
        return;

    if (console_state.mode == CONSOLE_VGA)
        _clear_vga(screen);
    else if (console_state.mode == CONSOLE_VESA)
        _clear_vesa(screen);

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
                _draw_char_vga(codepoint, col, row, attr);
            } else if (console_state.mode == CONSOLE_VESA) {
                u32 fg = ansi_rgb[cell->fg & 0x0f];
                u32 bg = ansi_rgb[cell->bg & 0x0f];
                _draw_char_vesa(codepoint, col, row, fg, bg);
            }
        }
    }
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

    _screen_update_vga_attr(screen);
}

static void _set_bg(console_screen_t* screen, u8 base, bool bright) {
    if (!screen)
        return;

    u8 idx = base & 0x7;
    if (bright)
        idx = (u8)(idx + 8);

    screen->bg_vga = idx;
    screen->bg_rgb = ansi_rgb[idx];

    _screen_update_vga_attr(screen);
}

static void _apply_sgr(console_screen_t* screen, int code) {
    if (!screen)
        return;

    if (code == 0) {
        _screen_reset_colors(screen);
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
    _free_screens();
    _init_screens(active);
    _redraw_screen(console_state.active_screen);
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
    case 2: {
        u8 r = (u8)((color >> 16) & 0xff);
        u8 g = (u8)((color >> 8) & 0xff);
        u8 b = (u8)(color & 0xff);
        u16 rgb565 = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        *(u16*)dst = rgb565;
        break;
    }
    default:
        break;
    }
}

static void _fill_rect(size_t x, size_t y, size_t width, size_t height, u32 color) {
    if (!console_state.fb_size || !width || !height)
        return;

    size_t width_bytes = width * console_state.bytes_per_pixel;
    size_t map_size = (height - 1) * console_state.pitch + width_bytes;
    size_t offset = y * console_state.pitch + x * console_state.bytes_per_pixel;

    u8* base = console_map_range(offset, map_size);
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

    u8* fb = console_map_range(0, console_state.fb_size);
    if (!fb)
        return;

    memmove(fb, fb + line_bytes, move_bytes);
    console_unmap_range(fb, console_state.fb_size);

    _fill_rect(
        0,
        console_state.height - console_state.font_height,
        console_state.width,
        console_state.font_height,
        screen->bg_rgb
    );
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
    u8* base = _map_range(offset, map_size);
    if (!base)
        return;

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

    _unmap_range(base, map_size);
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

            console_cell_t* cells = _screen_cells(screen_index);
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

    console_cell_t* cells = _screen_cells(screen_index);
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

static size_t _utf8_decode(const u8* data, size_t len, u32* out) {
    if (!data || !len || !out)
        return 0;

    u8 b0 = data[0];
    if (b0 < 0x80) {
        *out = b0;
        return 1;
    }

    if ((b0 & 0xe0) == 0xc0) {
        if (len < 2 || (data[1] & 0xc0) != 0x80)
            return 0;

        u32 cp = ((u32)(b0 & 0x1f) << 6) | (u32)(data[1] & 0x3f);
        if (cp < 0x80)
            return 0;

        *out = cp;
        return 2;
    }

    if ((b0 & 0xf0) == 0xe0) {
        if (len < 3 || (data[1] & 0xc0) != 0x80 || (data[2] & 0xc0) != 0x80)
            return 0;

        u32 cp = ((u32)(b0 & 0x0f) << 12) | ((u32)(data[1] & 0x3f) << 6) | (u32)(data[2] & 0x3f);
        if (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))
            return 0;

        *out = cp;
        return 3;
    }

    if ((b0 & 0xf8) == 0xf0) {
        if (len < 4 || (data[1] & 0xc0) != 0x80 || (data[2] & 0xc0) != 0x80 ||
            (data[3] & 0xc0) != 0x80)
            return 0;

        u32 cp = ((u32)(b0 & 0x07) << 18) | ((u32)(data[1] & 0x3f) << 12) |
                 ((u32)(data[2] & 0x3f) << 6) | (u32)(data[3] & 0x3f);
        if (cp < 0x10000 || cp > 0x10ffff)
            return 0;

        *out = cp;
        return 4;
    }

    return 0;
}

static void _write_screen(size_t screen_index, const char* buf, size_t len) {
    if (!buf || !len || !console_state.ready || console_state.mode == CONSOLE_DISABLED)
        return;

    console_screen_t* screen = _get_screen(screen_index);
    if (!screen)
        return;

    bool esc = false;
    bool csi = false;
    int params[8] = {0};
    size_t param_count = 0;
    int current = -1;

    for (size_t i = 0; i < len; i++) {
        u8 ch = (u8)buf[i];

        if (!esc) {
            if (ch == '\x1b') {
                esc = true;
                csi = false;
                continue;
            }

            if (ch < 0x20 || ch == 0x7f) {
                _putc(screen, screen_index, ch);
                continue;
            }

            if (ch < 0x80) {
                _putc(screen, screen_index, ch);
                continue;
            }

            u32 codepoint = 0;
            size_t consumed = _utf8_decode((const u8*)&buf[i], len - i, &codepoint);
            if (!consumed) {
                _putc(screen, screen_index, '?');
                continue;
            }

            _putc(screen, screen_index, codepoint);
            i += consumed - 1;
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
                if (current < 0 && param_count == 0) {
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
            esc = false;
            csi = false;
        }
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

void console_init(const boot_info_t* info) {
    if (!info)
        return;

    memset(&console_state, 0, sizeof(console_state));
    console_use_font(&default_font);

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

            _init_screens(TTY_CONSOLE);
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

            _init_screens(TTY_CONSOLE);
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

    if (len == 0)
        return 0;

    unsigned long flags = arch_irq_save();

    bool mirror = (screen == TTY_CONSOLE);
    if (!mirror && console_state.ready && screen == console_state.active_screen)
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
