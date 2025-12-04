#include "tty.h"

#include <base/attributes.h>
#include <base/types.h>
#include <stdio.h>
#include <x86/asm.h>
#include <x86/regs.h>
#include <x86/serial.h>

#include "bios.h"
#include "stdarg.h"


void puts(const char* str) {
    regs32_t regs = {.ah = 0x0e};

    while (*str) {
        regs.al = *str;
        send_serial(SERIAL_COM1, *str);
        bios_call(0x10, &regs, &regs);
        str++;
    }
}

int printf(char* fmt, ...) {
    char buf[PRINTF_BUF_SIZE];

    va_list args;
    va_start(args, fmt);

    int ret = vsprintf(buf, fmt, args);

    puts(buf);

    va_end(args);

    return ret;
}


NORETURN void panic(const char* msg) {
    puts("BOOTLOADER PANIC: ");
    puts(msg);
    puts("\n\rExecution halted!\n\r");

    halt();
    __builtin_unreachable();
}
