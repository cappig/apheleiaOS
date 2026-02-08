#include "physical.h"

#include <alloc/bitmap.h>
#include <base/macros.h>
#include <log/log.h>
#include <string.h>

#include "sys/panic.h"
#include "x86/asm.h"
#include "x86/boot.h"
#include "x86/e820.h"
#if defined(__x86_64__)
#include "x86/paging64.h"
#else
#include "x86/paging32.h"
#endif

static bitmap_allocator_t frame_alloc = {0};


void pmm_init(e820_map_t* mmap) {
    if (!bitmap_alloc_init_mmap(&frame_alloc, mmap, PAGE_4KIB))
        panic("Failed to initialize the page frame allocator!");
}

size_t pmm_total_mem(void) {
    return frame_alloc.block_count * frame_alloc.block_size;
}

size_t pmm_free_mem(void) {
    return frame_alloc.free_blocks * frame_alloc.block_size;
}


void* alloc_frames(size_t count) {
    assert(count);

    void* ret = bitmap_alloc_reserve(&frame_alloc, count);

    if (UNLIKELY(!ret))
        panic("Out of physical memory!");

#ifdef MMU_DEBUG
    log_debug("[MMU DEBUG] allocated %zu new frames: paddr = %#lx", count, (u64)ret);
#endif

    return ret;
}

void free_frames(void* ptr, size_t size) {
    bitmap_alloc_free(&frame_alloc, ptr, size);

#ifdef MMU_DEBUG
    log_debug("[MMU DEBUG] freed %zu frames: paddr = %#lx", size, (u64)ptr);
#endif
}


void reclaim_boot_map(e820_map_t* mmap) {
    for (size_t i = 0; i < mmap->count; i++) {
        e820_entry_t* current = &mmap->entries[i];

        if (current->type == E820_ALLOC)
            current->type = E820_AVAILABLE;
    }

    clean_mmap(mmap);

#if defined(__x86_64__)
    u64 root = read_cr3();
    page_t* root_vaddr = (page_t*)(root + LINEAR_MAP_OFFSET_64);

    // The higher half contains kernel mappings
    memset(root_vaddr, 0, 256 * sizeof(page_t));

    // Flush the TLB
    write_cr3(root);
#endif
}
