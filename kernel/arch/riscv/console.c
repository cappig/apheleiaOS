#include <arch/arch.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/console.h>

#include "console.h"
#include "serial.h"

#define RISCV_CONSOLE_COLS 80
#define RISCV_CONSOLE_ROWS 25

static uintptr_t console_uart_base = SERIAL_UART0;
static u16 text_shadow[RISCV_CONSOLE_COLS * RISCV_CONSOLE_ROWS];

static void _uart_printf(const char *fmt, ...) {
    char buf[64];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    send_serial_string(console_uart_base, buf);
}

static void _console_home(void) {
    send_serial_string(console_uart_base, "\x1b[H");
}

static void _console_clear(void) {
    send_serial_string(console_uart_base, "\x1b[2J");
    _console_home();
}

static void _console_repaint(void) {
    _console_clear();

    for (size_t row = 0; row < RISCV_CONSOLE_ROWS; row++) {
        for (size_t col = 0; col < RISCV_CONSOLE_COLS; col++) {
            u16 cell = text_shadow[row * RISCV_CONSOLE_COLS + col];
            char ch = (char)(cell & 0xff);
            if (!ch) {
                ch = ' ';
            }
            send_serial(console_uart_base, ch);
        }

        if (row + 1 < RISCV_CONSOLE_ROWS) {
            send_serial_string(console_uart_base, "\r\n");
        }
    }
}

static bool
_riscv_console_probe(void *arch_boot_info, console_hw_desc_t *out) {
    (void)arch_boot_info;

    if (!out) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->mode = CONSOLE_TEXT;
    out->fb = (u8 *)text_shadow;
    out->fb_size = sizeof(text_shadow);
    out->width = RISCV_CONSOLE_COLS;
    out->height = RISCV_CONSOLE_ROWS;
    out->pitch = RISCV_CONSOLE_COLS * sizeof(u16);
    out->bytes_per_pixel = sizeof(u16);
    return true;
}

static u8 *_riscv_fb_map(size_t offset, size_t size) {
    if (!size || offset >= sizeof(text_shadow)) {
        return NULL;
    }

    if (offset + size > sizeof(text_shadow)) {
        return NULL;
    }

    return (u8 *)text_shadow + offset;
}

static void _riscv_fb_unmap(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
}

static void _riscv_text_cursor_set(size_t col, size_t row) {
    _uart_printf("\x1b[%zu;%zuH", row + 1, col + 1);
}

static u16 _riscv_text_cell(u32 codepoint, u8 fg, u8 bg) {
    u8 ch = (codepoint > 0xff) ? (u8)'?' : (u8)codepoint;
    u8 attr = (u8)((bg << 4) | (fg & 0x0f));
    return ((u16)attr << 8) | ch;
}

static void _riscv_text_put(
    u8 *fb,
    size_t cols,
    size_t col,
    size_t row,
    u32 codepoint,
    u8 fg,
    u8 bg
) {
    if (!fb || !cols || col >= cols || row >= RISCV_CONSOLE_ROWS) {
        return;
    }

    u16 *text = (u16 *)fb;
    text[row * cols + col] = _riscv_text_cell(codepoint, fg, bg);

    char ch = (codepoint > 0xff) ? '?' : (char)codepoint;
    if (!ch) {
        ch = ' ';
    }

    _uart_printf("\x1b[%zu;%zuH%c", row + 1, col + 1, ch);
}

static void _riscv_text_clear(
    u8 *fb,
    size_t cols,
    size_t rows,
    u8 fg,
    u8 bg
) {
    if (!fb) {
        return;
    }

    u16 *text = (u16 *)fb;
    u16 blank = _riscv_text_cell(' ', fg, bg);
    size_t count = cols * rows;

    for (size_t i = 0; i < count; i++) {
        text[i] = blank;
    }

    _console_clear();
}

static void
_riscv_text_scroll_up(u8 *fb, size_t cols, size_t rows, u8 fg, u8 bg) {
    if (!fb || !cols || !rows) {
        return;
    }

    u16 *text = (u16 *)fb;
    u16 blank = _riscv_text_cell(' ', fg, bg);

    memmove(text, text + cols, (rows - 1) * cols * sizeof(*text));
    for (size_t col = 0; col < cols; col++) {
        text[(rows - 1) * cols + col] = blank;
    }

    _console_repaint();
}

static const console_backend_ops_t riscv_console_ops = {
    .probe = _riscv_console_probe,
    .fb_map = _riscv_fb_map,
    .fb_unmap = _riscv_fb_unmap,
    .text_cursor_set = _riscv_text_cursor_set,
    .text_put = _riscv_text_put,
    .text_clear = _riscv_text_clear,
    .text_scroll_up = _riscv_text_scroll_up,
};

void riscv_console_set_uart_base(uintptr_t uart_base) {
    if (uart_base) {
        console_uart_base = uart_base;
    }
}

uintptr_t riscv_console_uart_base(void) {
    return console_uart_base;
}

void riscv_console_backend_init(uintptr_t uart_base) {
    riscv_console_set_uart_base(uart_base);
    console_backend_register(&riscv_console_ops);
    _console_clear();
}
