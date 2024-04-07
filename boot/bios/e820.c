#include <base/types.h>
#include <x86/e820.h>

#include "bios.h"

#define SMAP 0x534d4150

// http://www.uruk.org/orig-grub/mem64mb.html
void get_e820(e820_map* mmap) {
    usize count = 0;

    regs out_regs = {0};
    regs in_regs = {
        .eax = 0xe820,
        .ecx = sizeof(e820_entry),
        .edx = SMAP,
    };

    while (count < E820_MAX) {
        e820_entry entry = {0};

        in_regs.edi = (u32)(uptr)&entry;

        bios_call(0x15, &in_regs, &out_regs);

        // Set continuation for next call
        in_regs.ebx = out_regs.ebx;

        // End of chain (or error?)
        if (out_regs.eflags & FLAG_CF || out_regs.eax != SMAP)
            break;

        // BIOS has fucked up; stop
        if (out_regs.ecx < 20 || out_regs.ecx > sizeof(e820_entry))
            break;

        mmap->entries[count] = entry;
        count++;

        // End of chain
        if (!out_regs.ebx)
            break;
    };

    mmap->count = count;
}
