#include "tty.h"

#include <base/attributes.h>
#include <base/types.h>
#include <stdio.h>
#include <x86/asm.h>
#include <x86/boot.h>
#include <x86/serial.h>
#include <x86/vga.h>

#include "stdarg.h"

#define ANSI_PARAM_CAP   4
#define VGA_ATTR_DEFAULT 0x07
#define VGA_CRTC_INDEX   0x3d4
#define VGA_CRTC_DATA    0x3d5
#define VGA_CURSOR_HIGH  0x0e
#define VGA_CURSOR_LOW   0x0f

typedef enum {
    ANSI_TEXT = 0,
    ANSI_ESCAPE,
    ANSI_CSI,
} ansi_state_t;

typedef struct {
    char log[BOOT_LOG_CAP];
    size_t log_len;
    bool bios_output;
    ansi_state_t ansi_state;
    u16 ansi_params[ANSI_PARAM_CAP];
    u16 ansi_value;
    u8 ansi_count;
    u8 vga_attr;
    u8 cursor_x;
    u8 cursor_y;
    bool ansi_value_set;
    bool cursor_ready;
} bios_tty_t;

static bios_tty_t bios_tty = {
    .bios_output = true,
    .vga_attr = VGA_ATTR_DEFAULT,
};

static const u8 ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

static volatile u16 *vga_buffer(void) {
    return (volatile u16 *)(uintptr_t)VGA_ADDR;
}

static void boot_log_putc(char c) {
    if (bios_tty.log_len >= BOOT_LOG_CAP) {
        return;
    }

    bios_tty.log[bios_tty.log_len++] = c;
}

const char *boot_log_buffer(size_t *len, size_t *cap) {
    if (len) {
        *len = bios_tty.log_len;
    }

    if (cap) {
        *cap = BOOT_LOG_CAP;
    }

    return bios_tty.log;
}

void tty_disable_bios_output(void) {
    bios_tty.bios_output = false;
}

static void cursor_init(void) {
    if (bios_tty.cursor_ready) {
        return;
    }

    outb(VGA_CRTC_INDEX, VGA_CURSOR_LOW);
    u16 pos = inb(VGA_CRTC_DATA);
    outb(VGA_CRTC_INDEX, VGA_CURSOR_HIGH);
    pos |= (u16)inb(VGA_CRTC_DATA) << 8;

    if (pos < VGA_WIDTH * VGA_HEIGHT) {
        bios_tty.cursor_x = (u8)(pos % VGA_WIDTH);
        bios_tty.cursor_y = (u8)(pos / VGA_WIDTH);
    }

    bios_tty.cursor_ready = true;
}

static void cursor_sync(void) {
    if (!bios_tty.cursor_ready) {
        return;
    }

    u16 pos = (u16)((u16)bios_tty.cursor_y * VGA_WIDTH + bios_tty.cursor_x);
    outb(VGA_CRTC_INDEX, VGA_CURSOR_LOW);
    outb(VGA_CRTC_DATA, (u8)pos);
    outb(VGA_CRTC_INDEX, VGA_CURSOR_HIGH);
    outb(VGA_CRTC_DATA, (u8)(pos >> 8));
}

static void vga_scroll(void) {
    volatile u16 *vga = vga_buffer();

    for (size_t y = 1; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            vga[(y - 1) * VGA_WIDTH + x] = vga[y * VGA_WIDTH + x];
        }
    }

    u16 blank = (u16)((u16)bios_tty.vga_attr << 8) | ' ';
    size_t last_row = (VGA_HEIGHT - 1) * VGA_WIDTH;
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        vga[last_row + x] = blank;
    }
}

static void bios_emit(char c) {
    cursor_init();

    if (c == '\r') {
        bios_tty.cursor_x = 0;
        return;
    }

    if (c == '\n') {
        bios_tty.cursor_y++;
    } else if (c == '\b') {
        if (bios_tty.cursor_x) {
            bios_tty.cursor_x--;
        }
        return;
    } else if ((u8)c >= ' ' && (u8)c != 0x7f) {
        size_t cell = (size_t)bios_tty.cursor_y * VGA_WIDTH + bios_tty.cursor_x;
        vga_buffer()[cell] = (u16)((u16)bios_tty.vga_attr << 8) | (u8)c;
        bios_tty.cursor_x++;

        if (bios_tty.cursor_x >= VGA_WIDTH) {
            bios_tty.cursor_x = 0;
            bios_tty.cursor_y++;
        }
    }

    if (bios_tty.cursor_y >= VGA_HEIGHT) {
        vga_scroll();
        bios_tty.cursor_y = VGA_HEIGHT - 1;
    }
}

static void set_sgr(u16 param) {
    u8 fg = bios_tty.vga_attr & 0x0f;
    u8 bg = bios_tty.vga_attr & 0xf0;

    if (param == 0) {
        bios_tty.vga_attr = VGA_ATTR_DEFAULT;
    } else if (param == 1) {
        bios_tty.vga_attr = bg | (fg | 0x08);
    } else if (param == 22) {
        bios_tty.vga_attr = bg | (fg & 0x07);
    } else if (param >= 30 && param <= 37) {
        bios_tty.vga_attr = bg | ansi_to_vga[param - 30] | (fg & 0x08);
    } else if (param == 39) {
        bios_tty.vga_attr = bg | 0x07;
    } else if (param >= 40 && param <= 47) {
        bios_tty.vga_attr = (u8)(ansi_to_vga[param - 40] << 4) | fg;
    } else if (param == 49) {
        bios_tty.vga_attr = fg;
    } else if (param >= 90 && param <= 97) {
        bios_tty.vga_attr = bg | ansi_to_vga[param - 90] | 0x08;
    }
}

static void ansi_reset(void) {
    bios_tty.ansi_state = ANSI_TEXT;
    bios_tty.ansi_value = 0;
    bios_tty.ansi_count = 0;
    bios_tty.ansi_value_set = false;
}

static void ansi_push(void) {
    if (bios_tty.ansi_count < ANSI_PARAM_CAP) {
        bios_tty.ansi_params[bios_tty.ansi_count++] = bios_tty.ansi_value_set ? bios_tty.ansi_value : 0;
    }

    bios_tty.ansi_value = 0;
    bios_tty.ansi_value_set = false;
}

static void bios_putc(char c) {
    if (bios_tty.ansi_state == ANSI_TEXT) {
        if (c == '\x1b') {
            bios_tty.ansi_state = ANSI_ESCAPE;
            return;
        }

        if (c == '\n') {
            bios_emit('\r');
        }
        bios_emit(c);
        return;
    }

    if (bios_tty.ansi_state == ANSI_ESCAPE) {
        if (c == '[') {
            bios_tty.ansi_state = ANSI_CSI;
            bios_tty.ansi_value = 0;
            bios_tty.ansi_count = 0;
            bios_tty.ansi_value_set = false;
        } else {
            ansi_reset();
        }
        return;
    }

    if (c >= '0' && c <= '9') {
        u16 digit = (u16)(c - '0');
        if (bios_tty.ansi_value <= 999) {
            bios_tty.ansi_value = (u16)(bios_tty.ansi_value * 10 + digit);
        }
        bios_tty.ansi_value_set = true;
        return;
    }

    if (c == ';') {
        ansi_push();
        return;
    }

    if (c == 'm') {
        if (bios_tty.ansi_value_set || !bios_tty.ansi_count) {
            ansi_push();
        }

        for (u8 i = 0; i < bios_tty.ansi_count; i++) {
            set_sgr(bios_tty.ansi_params[i]);
        }
    }

    ansi_reset();
}

int puts(const char *str) {
    int count = 0;

    while (*str) {
        char c = *str++;

        if (c == '\n') {
            send_serial(SERIAL_COM1, '\r');
        }

        boot_log_putc(c);
        send_serial(SERIAL_COM1, c);

        if (bios_tty.bios_output) {
            bios_putc(c);
        }

        count++;
    }

    if (bios_tty.bios_output) {
        cursor_sync();
    }

    return count;
}

int printf(const char *fmt, ...) {
    char buf[PRINTF_BUF_SIZE];

    va_list args;
    va_start(args, fmt);
    int length = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    puts(buf);

    return length;
}

NORETURN void panic(const char *msg) {
    puts("bootloader panic ");
    puts(msg);
    puts("\n\rexecution halted\n\r");

    halt();
    __builtin_unreachable();
}
