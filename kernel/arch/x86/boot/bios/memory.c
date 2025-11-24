#include "memory.h"

#include <string.h>

#include "bios.h"
// #include "stdlib.h"
#include "tty.h"
// #include "x86/lib/asm.h"


#define SMAP 0x534d4150


// http://www.uruk.org/orig-grub/mem64mb.html
void get_e820(e820_map* mmap) {
    regs out_regs = {0};
    regs in_regs = {
        .eax = 0xe820,
        .ecx = sizeof(e820_entry),
        .edx = SMAP,
    };

    while (mmap->count < E820_MAX) {
        e820_entry entry = {0};

        in_regs.edi = (u32)(uptr)&entry;

        bios_call(0x15, &in_regs, &out_regs);

        in_regs.ebx = out_regs.ebx;

        // End of chain, or error
        if (out_regs.eflags & FLAG_CF || out_regs.eax != SMAP)
            break;

        if (out_regs.ecx < 20 || out_regs.ecx > sizeof(e820_entry))
            break;

        mmap->entries[mmap->count] = entry;
        mmap->count++;

        // End of chain
        if (!out_regs.ebx)
            break;
    }

    printf("Got %d e820 memory regions from BIOS\n\r", mmap->count);
}


// https://wiki.osdev.org/RSDP#Detecting_the_RSDP
void get_rsdp(u64* rsdp) {
    u64 ebda = (u64)(*(u16*)0x40e << 4);

    for (u64 addr = ebda; addr <= 0xfffff; addr += 16) {
        // No RSDP found in the EBDA
        // Start searching the other possible memory region
        if (addr == ebda + 1024)
            addr = 0xe0000;

        if (!strncmp((char*)(uptr)addr, "RSD PTR ", 8)) {
            printf("RSDP is at %#p\n\r", addr);
            return;
        }
    }

    printf("No RSDP found\n\r");
}
