#include "memory.h"

#include <base/macros.h>
#include <base/types.h>
#include <stddef.h>
#include <string.h>
#include <x86/e820.h>

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
    };

    mmap->count = count;
    mmap_ptr = mmap;

    if (count == 0)
        panic("BIOS has returned an empty memory map!");

    // Reserve the first page of memory. It will never be used anyway
    mmap_add_entry(mmap, 0, 0x1000, E820_RESERVED);

    clean_mmap(mmap);
}


void* mmap_alloc(usize bytes, u32 type, uptr top) {
    void* ret = mmap_alloc_inner(mmap_ptr, bytes, type, top);

    if (ret == NULL)
        panic("Bootloader out of memory!");

    return ret;
}

void* boot_malloc(size_t size) {
    return mmap_alloc(size, E820_BOOT_ALLOC, (uptr)-1);
}

void* boot_calloc(size_t size) {
    void* ret = boot_malloc(size);
    memset(ret, 0, size);

    return ret;
}

// Use this when allocating buffers for disk reads
void* boot_malloc_low(size_t size) {
    return mmap_alloc(size, E820_BOOT_ALLOC, 0x100000);
}


void boot_free(void* ptr) {
    bool err = mmap_free_inner(mmap_ptr, ptr);

    if (err)
        panic("Attempted to free non allocated memory!");
}
