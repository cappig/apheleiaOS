#include <alloc/bitmap.h>
#include <base/addr.h>
#include <base/macros.h>
#include <string.h>
#include <x86/asm.h>
#include <x86/e820.h>
#include <x86/paging.h>

#include "log/log.h"
#include "sys/panic.h"

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
    assert(count);

    void* ret = bitmap_alloc_reserve(&frame_alloc, count);

    if (UNLIKELY(ret == NULL))
        panic("Out of physical memory!");

#ifdef MMU_DEBUG
    log_debug("[MMU DEBUG] allocated %zu new frames: paddr = %#lx", count, (u64)ret);
#endif

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

    // The higher half contains kernel mappings
    memset(root_vaddr, 0, 256 * sizeof(page_table));

    // This will flush the TLB
    write_cr3(root);
}

void dump_mem() {
    log_info("System has %zu MiB of usable RAM", get_total_mem() / MiB);
    log_info("%zu MiB are free for allocation", get_free_mem() / MiB);
}
