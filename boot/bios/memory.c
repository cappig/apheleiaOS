#include "memory.h"

#include <base/addr.h>
#include <base/macros.h>
#include <base/types.h>
#include <stddef.h>
#include <string.h>
#include <x86/e820.h>
#include <x86/paging.h>

#include "bios.h"
#include "tty.h"

#define SMAP 0x534d4150

static e820_map* mmap_ptr = NULL;


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
    }

    mmap->count = count;
    mmap_ptr = mmap;

    if (count == 0)
        panic("BIOS has returned an empty memory map!");

    // Reserve the first page of memory. It will never be used anyway
    mmap_add_entry(mmap, 0, 0x1000, E820_RESERVED);

    clean_mmap(mmap);
}


u64 alloc_kernel_stack(usize size) {
    void* stack = mmap_alloc(size, E820_KERNEL, PAGE_4KIB);

    return ID_MAPPED_VADDR((u64)(uptr)stack + size);
}


static void* _mmap_alloc_top(usize bytes, u32 type, u32 alignment, uptr top) {
    void* ret = mmap_alloc_inner(mmap_ptr, bytes, type, alignment, top);

    if (ret == NULL)
        panic("Bootloader out of memory!");

    return ret;
}

void* mmap_alloc(usize bytes, u32 type, u32 alignment) {
    return _mmap_alloc_top(bytes, type, alignment, (uptr)-1);
}

void* bmalloc_aligned(usize size, u32 alignment, bool allow_high) {
    uptr top = allow_high ? (uptr)-1 : 0x100000;
    void* ret = _mmap_alloc_top(size, E820_ALLOC, alignment, top);

    return ret;
}

void* bmalloc(usize size, bool allow_high) {
    return bmalloc_aligned(size, 1, allow_high);
}

void bfree(void* ptr) {
    if (mmap_free_inner(mmap_ptr, ptr))
        panic("Attempted to free non allocated memory!");
}


// https://wiki.osdev.org/RSDP#Detecting_the_RSDP
u64 get_rsdp() {
    u64 ebda = (u64)(*(u16*)0x40e << 4);

    for (u64 addr = ebda; addr <= 0xfffff; addr += 16) {
        // No RSDP found in the EBDA; start searching the
        // other possible memory region
        if (addr == ebda + 1024)
            addr = 0xe0000;

        if (!strncmp((char*)(uptr)addr, "RSD PTR ", 8)) {
            return addr;
        }
    }

    return 0;
}
