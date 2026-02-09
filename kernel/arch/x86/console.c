#include "console.h"

#include <arch/arch.h>
#include <base/macros.h>
#include <base/types.h>
#include <stddef.h>
#include <string.h>
#include <x86/serial.h>
#include <x86/vga.h>

#include "font.h"

typedef enum {
    CONSOLE_DISABLED = 0,
    CONSOLE_VGA = 1,
    CONSOLE_VESA = 2,
} console_mode_t;

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
    size_t cursor_x;
    size_t cursor_y;
    u8 vga_attr;
    u8 fg_vga;
    u8 bg_vga;
    u32 fg_rgb;
    u32 bg_rgb;
    bool bright;
    bool use_phys_window;
    bool ready;
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

static void _update_vga_attr(void) {
    console_state.vga_attr = (u8)((console_state.bg_vga << 4) | (console_state.fg_vga & 0x0f));
}

static void _reset_colors(void) {
    console_state.bright = false;
    console_state.fg_vga = VGA_GREY;
    console_state.bg_vga = VGA_BLACK;
    console_state.fg_rgb = ansi_rgb[console_state.fg_vga];
    console_state.bg_rgb = ansi_rgb[console_state.bg_vga];

    _update_vga_attr();
}

static void _set_fg(u8 base, bool force_bright) {
    u8 idx = base & 0x7;
    if (force_bright || console_state.bright)
        idx = (u8)(idx + 8);

    console_state.fg_vga = idx;
    console_state.fg_rgb = ansi_rgb[idx];

    _update_vga_attr();
}

static void _set_bg(u8 base, bool bright) {
    u8 idx = base & 0x7;
    if (bright)
        idx = (u8)(idx + 8);

    console_state.bg_vga = idx;
    console_state.bg_rgb = ansi_rgb[idx];

    _update_vga_attr();
}

static void _apply_sgr(int code) {
    if (code == 0) {
        _reset_colors();
        return;
    }

    if (code == 1) {
        console_state.bright = true;
        _set_fg(console_state.fg_vga & 0x7, true);
        return;
    }

    if (code == 2 || code == 22) {
        console_state.bright = false;
        _set_fg(console_state.fg_vga & 0x7, false);
        return;
    }

    if (code == 39) {
        _set_fg(VGA_GREY, false);
        return;
    }

    if (code == 49) {
        _set_bg(VGA_BLACK, false);
        return;
    }

    if (code >= 30 && code <= 37) {
        _set_fg((u8)(code - 30), false);
        return;
    }

    if (code >= 90 && code <= 97) {
        _set_fg((u8)(code - 90), true);
        return;
    }

    if (code >= 40 && code <= 47) {
        _set_bg((u8)(code - 40), false);
        return;
    }

    if (code >= 100 && code <= 107)
        _set_bg((u8)(code - 100), true);
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

static void _clear_vga(void) {
    if (!console_state.fb)
        return;

    u16* vga = (u16*)console_state.fb;
    u16 entry = ((u16)console_state.vga_attr << 8) | ' ';

    for (size_t i = 0; i < console_state.cols * console_state.rows; i++)
        vga[i] = entry;
}

static void _clear_vesa(void) {
    if (!console_state.fb_size)
        return;

    u8* fb = _map_range(0, console_state.fb_size);
    if (!fb)
        return;

    memset(fb, 0, console_state.fb_size);
    _unmap_range(fb, console_state.fb_size);
}

static void _scroll_vga(void) {
    if (!console_state.fb)
        return;

    u16* vga = (u16*)console_state.fb;
    size_t row_len = console_state.cols;
    size_t rows = console_state.rows;

    memmove(vga, vga + row_len, (rows - 1) * row_len * sizeof(u16));

    u16 entry = ((u16)console_state.vga_attr << 8) | ' ';
    for (size_t i = (rows - 1) * row_len; i < rows * row_len; i++)
        vga[i] = entry;
}

static void _scroll_vesa(void) {
    if (!console_state.fb_size)
        return;

    size_t line_bytes = console_state.pitch * CONSOLE_FONT_HEIGHT;
    size_t move_bytes = console_state.pitch * (console_state.height - CONSOLE_FONT_HEIGHT);

    u8* fb = _map_range(0, console_state.fb_size);
    if (!fb)
        return;

    memmove(fb, fb + line_bytes, move_bytes);
    memset(fb + move_bytes, 0, line_bytes);
    _unmap_range(fb, console_state.fb_size);
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

static void _draw_char_vesa(char ch, size_t col, size_t row) {
    if (!console_state.fb_size)
        return;

    size_t x = col * CONSOLE_FONT_WIDTH;
    size_t y = row * CONSOLE_FONT_HEIGHT;
    size_t width_bytes = CONSOLE_FONT_WIDTH * console_state.bytes_per_pixel;
    size_t map_size = (CONSOLE_FONT_HEIGHT - 1) * console_state.pitch + width_bytes;

    u8 index = (ch < 32 || ch > 126) ? 0 : (u8)(ch - 32);
    const u8* glyph = console_font_bitmap[index];

    size_t offset = y * console_state.pitch + x * console_state.bytes_per_pixel;
    u8* base = _map_range(offset, map_size);
    if (!base)
        return;

    for (size_t gy = 0; gy < CONSOLE_FONT_HEIGHT; gy++) {
        u8 bits = glyph[gy];
        u8* row_ptr = base + gy * console_state.pitch;

        for (size_t gx = 0; gx < CONSOLE_FONT_WIDTH; gx++) {
            u8 mask = (u8)(1 << (7 - gx));
            u32 color = (bits & mask) ? console_state.fg_rgb : console_state.bg_rgb;
            _write_pixel(row_ptr + gx * console_state.bytes_per_pixel, color);
        }
    }

    _unmap_range(base, map_size);
}

static void _draw_char_vga(char ch, size_t col, size_t row) {
    if (!console_state.fb)
        return;

    u16* vga = (u16*)console_state.fb;
    size_t index = row * console_state.cols + col;
    u16 entry = ((u16)console_state.vga_attr << 8) | (u8)ch;
    vga[index] = entry;
}

static void _newline(void) {
    console_state.cursor_x = 0;
    console_state.cursor_y++;

    if (console_state.cursor_y < console_state.rows)
        return;

    console_state.cursor_y = console_state.rows - 1;

    if (console_state.mode == CONSOLE_VGA)
        _scroll_vga();
    else if (console_state.mode == CONSOLE_VESA)
        _scroll_vesa();
}

static void _putc(char ch) {
    if (!console_state.ready || console_state.mode == CONSOLE_DISABLED)
        return;

    if (ch == '\n') {
        _newline();
        return;
    }

    if (ch == '\r') {
        console_state.cursor_x = 0;
        return;
    }

    if (ch == '\t') {
        size_t next = (console_state.cursor_x + 4) & ~3ULL;
        while (console_state.cursor_x < next)
            _putc(' ');

        return;
    }

    if (ch == '\b') {
        if (console_state.cursor_x > 0) {
            console_state.cursor_x--;

            if (console_state.mode == CONSOLE_VGA)
                _draw_char_vga(' ', console_state.cursor_x, console_state.cursor_y);
            else if (console_state.mode == CONSOLE_VESA)
                _draw_char_vesa(' ', console_state.cursor_x, console_state.cursor_y);
        }

        return;
    }

    if (console_state.cursor_x >= console_state.cols)
        _newline();

    if (console_state.mode == CONSOLE_VGA)
        _draw_char_vga(ch, console_state.cursor_x, console_state.cursor_y);
    else if (console_state.mode == CONSOLE_VESA)
        _draw_char_vesa(ch, console_state.cursor_x, console_state.cursor_y);

    console_state.cursor_x++;
}

static void _write(const char* buf, size_t len) {
    if (!buf || !console_state.ready || console_state.mode == CONSOLE_DISABLED)
        return;

    bool esc = false;
    bool csi = false;
    int params[8] = {0};
    size_t param_count = 0;
    int current = -1;

    for (size_t i = 0; i < len; i++) {
        char ch = buf[i];

        if (!esc) {
            if (ch == '\x1b') {
                esc = true;
                csi = false;
                continue;
            }

            _putc(ch);
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
                _apply_sgr(params[p]);

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

void console_init(const boot_info_t* info) {
    if (!info)
        return;

    memset(&console_state, 0, sizeof(console_state));
    _reset_colors();

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

            console_state.cols = console_state.width / CONSOLE_FONT_WIDTH;
            console_state.rows = console_state.height / CONSOLE_FONT_HEIGHT;
            if (!console_state.cols || !console_state.rows) {
                memset(&console_state, 0, sizeof(console_state));
                goto fallback_vga;
            }

            console_state.mode = CONSOLE_VESA;
            console_state.use_phys_window = true;
            console_state.ready = true;

            _clear_vesa();
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

            console_state.cols = console_state.width / CONSOLE_FONT_WIDTH;
            console_state.rows = console_state.height / CONSOLE_FONT_HEIGHT;
            if (!console_state.cols || !console_state.rows) {
                arch_phys_unmap(console_state.fb, size);
                memset(&console_state, 0, sizeof(console_state));
                goto fallback_vga;
            }

            console_state.mode = CONSOLE_VESA;
            console_state.ready = true;

            _clear_vesa();
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

    _clear_vga();
}

ssize_t arch_console_read(void* buf, size_t len) {
    if (!buf)
        return -1;

    u8* out = buf;

    for (size_t i = 0; i < len; i++)
        out[i] = (u8)receive_serial(SERIAL_COM1);

    return (ssize_t)len;
}

ssize_t arch_console_write(const void* buf, size_t len) {
    if (!buf)
        return -1;

    send_serial_sized_string(SERIAL_COM1, buf, len);
    _write(buf, len);
    return (ssize_t)len;
}

ssize_t arch_tty_read(void* buf, size_t len) {
    return arch_console_read(buf, len);
}

ssize_t arch_tty_write(const void* buf, size_t len) {
    return arch_console_write(buf, len);
}
