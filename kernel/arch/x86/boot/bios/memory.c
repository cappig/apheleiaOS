#include "memory.h"

#include <alloc/bitmap.h>
#include <base/macros.h>
#include <data/bitmap.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bios.h"
// #include "stdlib.h"

#include "stdlib.h"
#include "tty.h"
#include "x86/e820.h"


#define SMAP 0x534d4150

static e820_map_t* e820_mmap = NULL;

// http://www.uruk.org/orig-grub/mem64mb.html
void get_e820(e820_map_t* mmap) {
    regs32_t out_regs = {0};
    regs32_t in_regs = {
        .eax = 0xe820,
        .ecx = sizeof(e820_entry_t),
        .edx = SMAP,
    };

    while (mmap->count < E820_MAX) {
        e820_entry_t entry = {0};

        in_regs.edi = REAL_OFF(&entry);
        in_regs.es = REAL_SEG(&entry);

        bios_call(0x15, &in_regs, &out_regs);

        in_regs.ebx = out_regs.ebx;

        // End of chain, or error
        if (out_regs.eflags & FLAG_CF || out_regs.eax != SMAP)
            break;

        if (out_regs.ecx < 20 || out_regs.ecx > sizeof(e820_entry_t))
            break;

        mmap->entries[mmap->count] = entry;
        mmap->count++;

        // End of chain
        if (!out_regs.ebx)
            break;
    }

    e820_mmap = mmap;

    printf("Got %d e820 memory regions from BIOS\n\r", mmap->count);
}


// https://wiki.osdev.org/RSDP#Detecting_the_RSDP
void get_rsdp(u64* rsdp) {
    u16 ebda_seg = 0;
    asm volatile("movw 0x40e, %0" : "=r"(ebda_seg));
    u64 ebda = ((u64)ebda_seg) << 4;

    for (u64 addr = ebda; addr <= 0xfffff; addr += 16) {
        // No RSDP found in the EBDA
        // Start searching the other possible memory region
        if (addr == ebda + 1024)
            addr = 0xe0000;

        if (!strncmp((char*)(uintptr_t)addr, "RSD PTR ", 8)) {
            printf("RSDP is at %#p\n\r", addr);
            *rsdp = addr;

            return;
        }
    }

    printf("No RSDP found\n\r");
}


void* mmap_alloc(size_t size, int type, size_t alignment) {
    if (!size)
        panic("Attempetd to allocate zero bytes!");

    void* ret = mmap_alloc_inner(e820_mmap, size, type, alignment, (size_t)-1);

    if (!ret)
        panic("Out of memory!");

    return ret;
}

void* mmap_alloc_top(size_t size, int type, size_t alignment, u64 top) {
    if (!size)
        panic("Attempetd to allocate zero bytes!");

    void* ret = mmap_alloc_inner(e820_mmap, size, type, alignment, top);

    if (!ret)
        panic("Out of memory!");

    return ret;
}

static void* _balloc(size_t size) {
    return mmap_alloc(size, E820_ALLOC, 1);
}

static void _bfree(void* ptr) {
    if (mmap_free_inner(e820_mmap, ptr))
        panic("Attempted to free non allocated memory!");
}


static struct _external_alloc external_alloc = {0};
struct _external_alloc* _external_alloc = NULL;

void init_malloc() {
    _external_alloc = &external_alloc;

    _external_alloc->malloc = _balloc;
    _external_alloc->free = _bfree;
}
