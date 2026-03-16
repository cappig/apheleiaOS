#include "tty.h"

#include <base/attributes.h>
#include <base/types.h>
#include <riscv/asm.h>
#include <riscv/serial.h>
#include <stdarg.h>
#include <stdio.h>

int puts(const char *str) {
    int count = 0;

    while (*str) {
        send_serial(SERIAL_UART0, *str++);
        count++;
    }

    return count;
}

void serial_puts(const char *str) {
    send_serial_string(SERIAL_UART0, str);
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

int serial_printf(const char *fmt, ...) {
    char buf[PRINTF_BUF_SIZE];

    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    serial_puts(buf);

    return ret;
}

NORETURN void panic(const char *msg) {
    puts("bootloader panic ");
    puts(msg);
    puts("\n\rexecution halted\n\r");

    halt();
    __builtin_unreachable();
}
