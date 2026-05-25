#include "tty.h"

#include <base/attributes.h>
#include <base/types.h>
#include <lib/boot.h>
#include <riscv/asm.h>
#include <riscv/serial.h>
#include <stdarg.h>
#include <stdio.h>

static uintptr_t tty_uart_base = SERIAL_UART0;
static char boot_log_buf[BOOT_LOG_CAP];
static size_t boot_log_len = 0;

static void boot_log_putc(char c) {
    if (boot_log_len >= BOOT_LOG_CAP) {
        return;
    }

    boot_log_buf[boot_log_len++] = c;
}

const char *boot_log_buffer(size_t *len, size_t *cap) {
    if (len) {
        *len = boot_log_len;
    }

    if (cap) {
        *cap = BOOT_LOG_CAP;
    }

    return boot_log_buf;
}

void tty_set_uart_base(uintptr_t base) {
    tty_uart_base = base;
}

int puts(const char *str) {
    int count = 0;

    while (*str) {
        char c = *str++;

        if (c == '\n') {
            send_serial(tty_uart_base, '\r');
        }

        boot_log_putc(c);
        send_serial(tty_uart_base, c);
        count++;
    }

    return count;
}

int printf(const char *fmt, ...) {
    char buf[PRINTF_BUF_SIZE];

    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    puts(buf);

    return ret;
}

NORETURN void panic(const char *msg) {
    puts("bootloader panic ");
    puts(msg);
    puts("\nexecution halted\n");

    halt();
    __builtin_unreachable();
}
