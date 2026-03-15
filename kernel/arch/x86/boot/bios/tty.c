#include "tty.h"

#include <base/attributes.h>
#include <base/types.h>
#include <stdio.h>
#include <x86/asm.h>
#include <x86/boot.h>
#include <x86/regs.h>
#include <x86/serial.h>

#include "bios.h"
#include "stdarg.h"

static char boot_log_buf[BOOT_LOG_CAP];
static size_t boot_log_len = 0;

static void _boot_log_putc(char c) {
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

int puts(const char *str) {
    regs32_t regs = {.ah = 0x0e};
    int count = 0;

    while (*str) {
        regs.al = *str;
        _boot_log_putc(*str);
        send_serial(SERIAL_COM1, *str);
        bios_call(0x10, &regs, &regs);
        str++;
        count++;
    }

    return count;
}

void serial_puts(const char *str) {
    while (*str) {
        _boot_log_putc(*str);
        send_serial(SERIAL_COM1, *str);
        str++;
    }
}

int printf(const char *fmt, ...) {
    char buf[PRINTF_BUF_SIZE];

    va_list args;
    va_start(args, fmt);

    int ret = vsnprintf(buf, sizeof(buf), fmt, args);

    puts(buf);

    va_end(args);

    return ret;
}

int serial_printf(const char *fmt, ...) {
    char buf[PRINTF_BUF_SIZE];

    va_list args;
    va_start(args, fmt);

    int ret = vsnprintf(buf, sizeof(buf), fmt, args);

    serial_puts(buf);

    va_end(args);

    return ret;
}

NORETURN void panic(const char *msg) {
    puts("bootloader panic ");
    puts(msg);
    puts("\n\rexecution halted\n\r");

    halt();
    __builtin_unreachable();
}
