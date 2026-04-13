#include "tty.h"

#include <base/attributes.h>
#include <base/types.h>
#include <riscv/asm.h>
#include <riscv/serial.h>
#include <stdarg.h>
#include <stdio.h>

static uintptr_t tty_uart_base = SERIAL_UART0;

void tty_set_uart_base(uintptr_t base) {
    tty_uart_base = base;
}

int puts(const char *str) {
    int count = 0;

    while (*str) {
        send_serial(tty_uart_base, *str++);
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
