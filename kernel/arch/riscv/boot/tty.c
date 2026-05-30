#include "tty.h"

#include <base/attributes.h>
#include <base/types.h>
#include <lib/boot.h>
#include <riscv/asm.h>
#include <riscv/serial.h>
#include <stdarg.h>
#include <stdio.h>

typedef struct {
    uintptr_t uart;
    char log[BOOT_LOG_CAP];
    size_t log_len;
} boot_tty_t;

static boot_tty_t boot_tty = {
    .uart = SERIAL_UART0,
};

static void boot_log_putc(char c) {
    if (boot_tty.log_len >= BOOT_LOG_CAP) {
        return;
    }

    boot_tty.log[boot_tty.log_len++] = c;
}

const char *boot_log_buffer(size_t *len, size_t *cap) {
    if (len) {
        *len = boot_tty.log_len;
    }

    if (cap) {
        *cap = BOOT_LOG_CAP;
    }

    return boot_tty.log;
}

void tty_set_uart_base(uintptr_t base) {
    boot_tty.uart = base;
}

int puts(const char *str) {
    int count = 0;

    while (*str) {
        char c = *str++;

        if (c == '\n') {
            send_serial(boot_tty.uart, '\r');
        }

        boot_log_putc(c);
        send_serial(boot_tty.uart, c);
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
