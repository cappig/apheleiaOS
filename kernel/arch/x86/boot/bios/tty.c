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
static bool bios_output_enabled = true;

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

void tty_disable_bios_output(void) {
    bios_output_enabled = false;
}

int puts(const char *str) {
    regs32_t regs = { .ah = 0x0e };
    int count = 0;

    while (*str) {
        char c = *str++;

        if (c == '\n') {
            send_serial(SERIAL_COM1, '\r');

            if (bios_output_enabled) {
                regs.al = '\r';
                bios_call(0x10, &regs, &regs);
            }
        }

        boot_log_putc(c);
        send_serial(SERIAL_COM1, c);

        if (bios_output_enabled) {
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
