#include <arch/arch.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/console.h>

#include "console.h"
#include "serial.h"

#define COLS 80
#define ROWS 25

static uintptr_t uart_base = SERIAL_UART0;

static u16 text_shadow[COLS * ROWS];

static size_t cur_col = 0;
static size_t cur_row = 0;
static bool cursor_valid = false;
static bool wrap_pending = false;
static bool suppressed = false;

static void _emit_escape(const char *fmt, ...) {
    char buf[64];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (!suppressed) {
        send_serial_string(uart_base, buf);
    }
}

static void _put(char ch) {
    if (!suppressed) {
        send_serial(uart_base, ch);
    }

    if (!cursor_valid) {
        return;
    }

    if (ch == '\r') {
        cur_col = 0;
        wrap_pending = false;
        return;
    }

    if (ch == '\n') {
        cur_col = 0;
        if (cur_row + 1 < ROWS) {
            cur_row++;
        }
        wrap_pending = false;
        return;
    }

    if (wrap_pending) {
        cur_col = 0;
        if (cur_row + 1 < ROWS) {
            cur_row++;
        }
        wrap_pending = false;
    }

    if (cur_col + 1 < COLS) {
        cur_col++;
        return;
    }

    wrap_pending = true;
}

static void _move_up(size_t n)    { if (n) { _emit_escape("\x1b[%zuA", n); } }
static void _move_down(size_t n)  { if (n) { _emit_escape("\x1b[%zuB", n); } }
static void _move_left(size_t n)  { if (n) { _emit_escape("\x1b[%zuD", n); } }
static void _move_right(size_t n) { if (n) { _emit_escape("\x1b[%zuC", n); } }

static void _sync_cursor(size_t col, size_t row) {
    if (col >= COLS) {
        col = COLS - 1;
    }
    if (row >= ROWS) {
        row = ROWS - 1;
    }

    if (suppressed || !cursor_valid) {
        cur_col = col;
        cur_row = row;
        cursor_valid = true;
        wrap_pending = false;
        return;
    }

    bool same = cur_col == col && cur_row == row;
    bool wrapped_next =
        wrap_pending && col == 0 &&
        ((cur_row + 1 < ROWS && row == cur_row + 1) ||
         (cur_row + 1 >= ROWS && row == ROWS - 1));

    if (same || wrapped_next) {
        return;
    }

    if (row == cur_row && col == 0) {
        _put('\r');
        return;
    }

    if (col == 0 && row > cur_row) {
        _put('\r');
        while (cur_row < row) {
            _put('\n');
        }
        return;
    }

    if (row < cur_row) {
        _move_up(cur_row - row);
    } else if (row > cur_row) {
        _move_down(row - cur_row);
    }

    if (col < cur_col) {
        _move_left(cur_col - col);
    } else if (col > cur_col) {
        _move_right(col - cur_col);
    }

    cur_col = col;
    cur_row = row;
    cursor_valid = true;
    wrap_pending = false;
}

static bool _probe(void *arch_boot_info, console_hw_desc_t *out) {
    (void)arch_boot_info;

    if (!out) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->mode = CONSOLE_TEXT;
    out->fb = (u8 *)text_shadow;
    out->fb_size = sizeof(text_shadow);
    out->width = COLS;
    out->height = ROWS;
    out->pitch = COLS * sizeof(u16);
    out->bytes_per_pixel = sizeof(u16);
    return true;
}

static u8 *_fb_map(size_t offset, size_t size) {
    if (!size || offset >= sizeof(text_shadow)) {
        return NULL;
    }
    if (offset + size > sizeof(text_shadow)) {
        return NULL;
    }
    return (u8 *)text_shadow + offset;
}

static void _fb_unmap(void *ptr, size_t size) {
    (void)ptr;
    (void)size;
}

static void _set_suppressed(bool val) {
    uart_console_set_suppressed(val);
}

static ssize_t _stream_write(const void *buf, size_t len) {
    if (!buf) {
        return -1;
    }
    if (!len) {
        return 0;
    }

    send_serial_sized_string(uart_base, buf, len);
    return (ssize_t)len;
}

static void _cursor_set(size_t col, size_t row) {
    _sync_cursor(col, row);
}

static u16 _cell(u32 codepoint, u8 fg, u8 bg) {
    u8 ch = (codepoint > 0xff) ? (u8)'?' : (u8)codepoint;
    u8 attr = (u8)((bg << 4) | (fg & 0x0f));
    return ((u16)attr << 8) | ch;
}

static void _text_put(
    u8 *fb,
    size_t cols,
    size_t col,
    size_t row,
    u32 codepoint,
    u8 fg,
    u8 bg
) {
    if (!fb || !cols || col >= cols || row >= ROWS) {
        return;
    }

    u16 *text = (u16 *)fb;
    text[row * cols + col] = _cell(codepoint, fg, bg);

    char ch = (codepoint > 0xff) ? '?' : (char)codepoint;
    if (!ch) {
        ch = ' ';
    }

    _sync_cursor(col, row);
    _put(ch);
}

static void _text_clear(u8 *fb, size_t cols, size_t rows, u8 fg, u8 bg) {
    if (!fb) {
        return;
    }

    u16 *text = (u16 *)fb;
    u16 blank = _cell(' ', fg, bg);
    size_t count = cols * rows;

    for (size_t i = 0; i < count; i++) {
        text[i] = blank;
    }

    if (!suppressed) {
        send_serial_string(uart_base, "\x1b[2J\x1b[H");
    }

    cur_col = 0;
    cur_row = 0;
    cursor_valid = true;
    wrap_pending = false;
}

static void _text_scroll_up(u8 *fb, size_t cols, size_t rows, u8 fg, u8 bg) {
    if (!fb || !cols || !rows) {
        return;
    }

    u16 *text = (u16 *)fb;
    u16 blank = _cell(' ', fg, bg);

    memmove(text, text + cols, (rows - 1) * cols * sizeof(*text));
    for (size_t col = 0; col < cols; col++) {
        text[(rows - 1) * cols + col] = blank;
    }

    _sync_cursor(0, rows - 1);
    _put('\r');
    _put('\n');
}

static const console_backend_ops_t uart_console_ops = {
    .probe = _probe,
    .fb_map = _fb_map,
    .fb_unmap = _fb_unmap,
    .set_output_suppressed = _set_suppressed,
    .stream_write = _stream_write,
    .text_cursor_set = _cursor_set,
    .text_put = _text_put,
    .text_clear = _text_clear,
    .text_scroll_up = _text_scroll_up,
};

void uart_console_set_base(uintptr_t base) {
    uart_base = base;
}

uintptr_t uart_console_base(void) {
    return uart_base;
}

void uart_console_set_suppressed(bool val) {
    suppressed = val;
}

void uart_console_init(uintptr_t base) {
    uart_console_set_base(base);
    console_backend_register(&uart_console_ops);
}
