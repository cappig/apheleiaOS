#include "memory.h"

#include <alloc/bitmap.h>
#include <base/macros.h>
#include <data/bitmap.h>
#include <stdint.h>
#include <string.h>

#include "bios.h"
// #include "stdlib.h"

#include "tty.h"
#include "x86/lib/paging.h"


#define SMAP 0x534d4150


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

        in_regs.edi = (u32)(uintptr_t)&entry;

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

        if (!strncmp((char*)addr, "RSD PTR ", 8)) {
            printf("RSDP is at %#p\n\r", addr);
            *rsdp = addr;

            return;
        }
    }

    printf("No RSDP found\n\r");
}


bool bitmap_alloc_init_mmap(bitmap_allocator_t* alloc, e820_map_t* mmap, size_t block_size) {
    u32 mem_base = (u32)-1;
    u32 mem_top = 0;

    for (size_t i = 0; i < mmap->count; i++) {
        e820_entry_t* current = &mmap->entries[i];

        if (current->type != E820_AVAILABLE)
            continue;

        u32 top = current->address + current->size;
        u32 base = current->address;

        if (mem_base > base)
            mem_base = base;

        if (mem_top < top)
            mem_top = top;
    }

    u32 mem_size = mem_top - mem_base;

    // Shift the base up so that the addresses end up aligned to the size of the block
    mem_base = ALIGN(mem_base, block_size);

    alloc->chuck_start = (void*)(uintptr_t)mem_base;
    alloc->chunk_size = mem_size;

    alloc->block_size = block_size;
    alloc->block_count = mem_size / block_size;
    alloc->word_count = alloc->block_count / BITMAP_WORD_SIZE;

    size_t bitmap_bytes = alloc->block_count / CHAR_BIT;

    if (mem_size <= bitmap_bytes)
        return false;

    // Find some space for the bitmap
    void* bitmap_addr = mmap_alloc_inner(mmap, bitmap_bytes, E820_ALLOC, 1, 0);
    if (!bitmap_addr)
        return false;

    alloc->bitmap = (bitmap_word_t*)(bitmap_addr + LINEAR_MAP_OFFSET);

    // Mark the whole bitmap as used
    memset(alloc->bitmap, (unsigned int)-1, bitmap_bytes);
    alloc->free_blocks = 0;

    for (size_t i = 0; i < mmap->count; i++) {
        e820_entry_t* current = &mmap->entries[i];

        u32 top = current->address + current->size;
        u32 base = current->address;

        if (top > mem_top || base < mem_base)
            continue;

        size_t blocks = current->size / block_size;
        size_t start_block = bitmap_alloc_to_block(alloc, (void*)current->address);

        if (current->type == E820_AVAILABLE) {
            alloc->free_blocks += blocks;
            bitmap_clear_region(alloc->bitmap, start_block, blocks);
        } else {
            // Do we need this?
            bitmap_set_region(alloc->bitmap, start_block, blocks);
        }
    }

    return true;
}
