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

typedef struct {
    char log[BOOT_LOG_CAP];
    size_t log_len;
    bool bios_output;
} bios_tty_t;

static bios_tty_t bios_tty = {
    .bios_output = true,
};

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

int puts(const char *str) {
    regs32_t regs = { .ah = 0x0e };
    int count = 0;

    while (*str) {
        char c = *str++;

        if (c == '\n') {
            send_serial(SERIAL_COM1, '\r');

            if (bios_tty.bios_output) {
                regs.al = '\r';
                bios_call(0x10, &regs, &regs);
            }
        }

        boot_log_putc(c);
        send_serial(SERIAL_COM1, c);

        if (bios_tty.bios_output) {
            regs.al = c;
            bios_call(0x10, &regs, &regs);
        }

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
    puts("\n\rexecution halted\n\r");

    halt();
    __builtin_unreachable();
}
