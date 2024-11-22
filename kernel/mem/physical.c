#include <alloc/bitmap.h>
#include <base/addr.h>
#include <string.h>
#include <x86/asm.h>
#include <x86/e820.h>
#include <x86/paging.h>

#include "arch/panic.h"

static bitmap_alloc frame_alloc;


void pmm_init(e820_map* mmap) {
    bool ret = bitmap_alloc_init_mmap(&frame_alloc, mmap, PAGE_4KIB);

    if (!ret)
        panic("Failed to initialize the page frame allocator!");
}

usize get_total_mem() {
    return frame_alloc.block_count * frame_alloc.block_size;
}

usize get_free_mem() {
    return frame_alloc.free_blocks * frame_alloc.block_size;
}

void* alloc_frames(usize count) {
    void* ret = bitmap_alloc_blocks(&frame_alloc, count);

#ifdef MMU_DEBUG
    log_debug("[MMU DEBUG] allocated %zu new frames: paddr = %#lx", count, (u64)ret);
#endif

    assert(ret != NULL);

    return ret;
}

void free_frames(void* ptr, usize size) {
    bitmap_alloc_free(&frame_alloc, ptr, size);

#ifdef MMU_DEBUG
    log_debug("[MMU DEBUG] freed %zu frames: paddr = %#lx", size, (u64)ptr);
#endif
}

void reclaim_boot_map(e820_map* mmap) {
    for (usize i = 0; i < mmap->count; i++) {
        e820_entry* current = &mmap->entries[i];

        if (current->type == E820_PAGE_TABLE || current->type == E820_ALLOC)
            current->type = E820_AVAILABLE;
    }

    clean_mmap(mmap);

    u64 root = read_cr3();
    page_table* root_vaddr = (page_table*)ID_MAPPED_VADDR(root);

    // All of the memory backing these pages has been allocated in the mmap.
    // We reclaimed that just now. So we can just zero out the lower half of the
    // root table thus unmapping any memory witouth memory leaks.
    memset(root_vaddr, 0, 256 * sizeof(page_table));

    // This will flush the TLB
    write_cr3(root);
}
