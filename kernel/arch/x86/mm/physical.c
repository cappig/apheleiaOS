#include "physical.h"

#include <alloc/bitmap.h>
#include <base/macros.h>
#include <limits.h>
#include <log/log.h>
#include <stdlib.h>
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
static u16* frame_refs = NULL;
static size_t frame_refs_count = 0;
static bool frame_refs_ready = false;

static size_t _pmm_block_index(void* ptr) {
    return bitmap_alloc_to_block(&frame_alloc, ptr);
}

static void _pmm_ref_set_range(void* ptr, size_t blocks, u16 value) {
    if (!frame_refs_ready || !ptr || !blocks)
        return;

    size_t start = _pmm_block_index(ptr);

    for (size_t i = 0; i < blocks; i++) {
        size_t index = start + i;

        if (index < frame_refs_count)
            frame_refs[index] = value;
    }
}


void pmm_init(e820_map_t* mmap) {
    log_debug("initializing PMM");

    if (!bitmap_alloc_init_mmap(&frame_alloc, mmap, PAGE_4KIB))
        panic("Failed to initialize the page frame allocator!");

    log_debug("PMM ready");
}

void pmm_ref_init(void) {
    if (frame_refs_ready)
        return;

    if (!frame_alloc.block_count || !frame_alloc.bitmap)
        return;

    frame_refs = calloc(frame_alloc.block_count, sizeof(*frame_refs));
    if (!frame_refs) {
        log_warn("PMM: failed to allocate refcount table");
        return;
    }

    frame_refs_count = frame_alloc.block_count;

    for (size_t i = 0; i < frame_alloc.block_count; i++) {
        if (bitmap_get(frame_alloc.bitmap, i))
            frame_refs[i] = 1;
    }

    frame_refs_ready = true;
    log_debug("PMM: refcount table ready");
}

bool pmm_ref_ready(void) {
    return frame_refs_ready;
}

size_t pmm_total_mem(void) {
    return frame_alloc.usable_blocks * frame_alloc.block_size;
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

    _pmm_ref_set_range(ret, count, 1);
    return ret;
}

void* alloc_frames_high(size_t count) {
    assert(count);

    void* ret = bitmap_alloc_reserve_high(&frame_alloc, count);

    if (UNLIKELY(!ret))
        panic("Out of physical memory!");

#ifdef MMU_DEBUG
    log_debug("[MMU DEBUG] allocated %zu new frames (high): paddr = %#lx", count, (u64)ret);
#endif

    _pmm_ref_set_range(ret, count, 1);
    return ret;
}

void* alloc_frames_user(size_t count) {
#if defined(__i386__)
    return alloc_frames_high(count);
#else
    return alloc_frames(count);
#endif
}

void free_frames(void* ptr, size_t size) {
    if (!frame_refs_ready) {
        bitmap_alloc_free(&frame_alloc, ptr, size);
        return;
    }

    if (!ptr || !size)
        return;

    size_t start = _pmm_block_index(ptr);

    for (size_t i = 0; i < size; i++) {
        size_t index = start + i;

        if (index >= frame_refs_count)
            continue;

        if (frame_refs[index] > 0)
            frame_refs[index]--;

        if (!frame_refs[index])
            bitmap_alloc_free(&frame_alloc, bitmap_alloc_to_ptr(&frame_alloc, index), 1);
    }

#ifdef MMU_DEBUG
    log_debug("[MMU DEBUG] freed %zu frames: paddr = %#lx", size, (u64)ptr);
#endif
}

void pmm_ref_hold(void* ptr, size_t blocks) {
    if (!frame_refs_ready || !ptr || !blocks)
        return;

    size_t start = _pmm_block_index(ptr);

    for (size_t i = 0; i < blocks; i++) {
        size_t index = start + i;

        if (index < frame_refs_count && frame_refs[index] < UINT16_MAX)
            frame_refs[index]++;
    }
}

u16 pmm_refcount(void* ptr) {
    if (!frame_refs_ready || !ptr)
        return 1;

    size_t index = _pmm_block_index(ptr);

    if (index >= frame_refs_count)
        return 1;

    return frame_refs[index];
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

    // the higher half contains kernel mappings
    memset(root_vaddr, 0, 256 * sizeof(page_t));

    // flush the TLB
    write_cr3(root);
#endif
}
