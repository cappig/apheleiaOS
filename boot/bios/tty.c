#include <base/attributes.h>
#include <base/types.h>
#include <x86/asm.h>
#include <x86/regs.h>

#include "bios.h"


void puts(const char* str) {
    regs r = {.ah = 0x0e};

    while (*str) {
        r.al = *str;
        bios_call(0x10, &r, &r);
        str++;
    }
}

NORETURN void panic(const char* msg) {
    puts("BOOTLOADER PANIC: ");
    puts(msg);
    puts("\n\rExecution halted!");

    halt();
    __builtin_unreachable();
}
