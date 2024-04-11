#include <alloc/bitmap.h>
#include <base/addr.h>
#include <x86/asm.h>
#include <x86/e820.h>
#include <x86/paging.h>

#include "video/tty.h"

static bitmap_alloc frame_alloc = {0};


void pmm_init(e820_map* mmap) {
    if (!bitmap_alloc_init(&frame_alloc, mmap, PAGE_4KIB))
        panic("Failed to init page the bage farame allocator!");
}

void* alloc_frames(usize count) {
    return bitmap_alloc_blocks(&frame_alloc, count);
}

void free_frames(void* ptr, usize size) {
    bitmap_alloc_free(&frame_alloc, ptr, size);
}

void reclaim_boot_map(e820_map* mmap) {
    for (usize i = 0; i < mmap->count; i++) {
        e820_entry* current = &mmap->entries[i];

        if (current->type == E820_PAGE_TABLE || current->type == E820_ALLOC)
            current->type = E820_AVAILABLE;
    }
}
