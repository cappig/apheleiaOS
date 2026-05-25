#include "physical.h"

#include <alloc/bitmap.h>
#include <base/macros.h>
#include <inttypes.h>
#include <limits.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>

#include "sys/panic.h"
#include "x86/asm.h"
#include "x86/boot.h"
#include "x86/e820.h"
#if defined(__x86_64__)
#include "x86/paging64.h"
#else
#include "x86/paging32.h"
#endif

typedef struct {
    bitmap_allocator_t frames;
    u16 *refs;
    size_t ref_count;
    bool refs_ready;
    spinlock_t lock;
} pmm_state_t;

static pmm_state_t pmm = {
    .lock = SPINLOCK_INIT,
};

static size_t _pmm_block_index(void *ptr) {
    return bitmap_alloc_to_block(&pmm.frames, ptr);
}

static void _pmm_ref_set_range(void *ptr, size_t blocks, u16 value) {
    if (!pmm.refs_ready || !ptr || !blocks) {
        return;
    }

    size_t start = _pmm_block_index(ptr);

    for (size_t i = 0; i < blocks; i++) {
        size_t index = start + i;

        if (index < pmm.ref_count) {
            pmm.refs[index] = value;
        }
    }
}


void pmm_init(e820_map_t *mmap) {
    log_debug("initializing physical memory manager");
    unsigned long irq_flags = spin_lock_irqsave(&pmm.lock);

    if (!bitmap_alloc_init_mmap(&pmm.frames, mmap, PAGE_4KIB)) {
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        panic("Failed to initialize the page frame allocator!");
    }

    spin_unlock_irqrestore(&pmm.lock, irq_flags);
    log_debug("PMM ready");
}

void pmm_ref_init(void) {
    unsigned long irq_flags = spin_lock_irqsave(&pmm.lock);

    if (pmm.refs_ready || !pmm.frames.block_count || !pmm.frames.bitmap) {
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        return;
    }

    size_t block_count = pmm.frames.block_count;
    spin_unlock_irqrestore(&pmm.lock, irq_flags);

    u16 *refs = calloc(block_count, sizeof(*refs));
    if (!refs) {
        log_warn("failed to allocate refcount table");
        return;
    }

    irq_flags = spin_lock_irqsave(&pmm.lock);

    if (pmm.refs_ready) {
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        free(refs);
        return;
    }

    pmm.refs = refs;
    pmm.ref_count = pmm.frames.block_count;

    for (size_t i = 0; i < pmm.frames.block_count; i++) {
        if (bitmap_get(pmm.frames.bitmap, i)) {
            pmm.refs[i] = 1;
        }
    }

    pmm.refs_ready = true;
    spin_unlock_irqrestore(&pmm.lock, irq_flags);
    log_debug("refcount table ready");
}

bool pmm_ref_ready(void) {
    unsigned long irq_flags = spin_lock_irqsave(&pmm.lock);
    bool ready = pmm.refs_ready;
    spin_unlock_irqrestore(&pmm.lock, irq_flags);
    return ready;
}

size_t pmm_total_mem(void) {
    unsigned long irq_flags = spin_lock_irqsave(&pmm.lock);
    size_t total = pmm.frames.usable_blocks * pmm.frames.block_size;
    spin_unlock_irqrestore(&pmm.lock, irq_flags);
    return total;
}

size_t pmm_free_mem(void) {
    unsigned long irq_flags = spin_lock_irqsave(&pmm.lock);
    size_t free_mem = pmm.frames.free_blocks * pmm.frames.block_size;
    spin_unlock_irqrestore(&pmm.lock, irq_flags);
    return free_mem;
}


void *alloc_frames(size_t count) {
    assert(count);
    unsigned long irq_flags = spin_lock_irqsave(&pmm.lock);

    void *ret = bitmap_alloc_reserve(&pmm.frames, count);

    if (UNLIKELY(!ret)) {
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        panic("Out of physical memory!");
    }

#ifdef MMU_DEBUG
    log_debug("PMM allocated %zu frames paddr=%#" PRIx64, count, (u64)(uintptr_t)ret);
#endif

    _pmm_ref_set_range(ret, count, 1);
    spin_unlock_irqrestore(&pmm.lock, irq_flags);
    return ret;
}

void *alloc_frames_high(size_t count) {
    assert(count);
    unsigned long irq_flags = spin_lock_irqsave(&pmm.lock);

    void *ret = bitmap_alloc_reserve_high(&pmm.frames, count);

    if (UNLIKELY(!ret)) {
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        panic("Out of physical memory!");
    }

#ifdef MMU_DEBUG
    log_debug("PMM allocated %zu high frames paddr=%#" PRIx64, count, (u64)(uintptr_t)ret);
#endif

    _pmm_ref_set_range(ret, count, 1);
    spin_unlock_irqrestore(&pmm.lock, irq_flags);
    return ret;
}

void *alloc_frames_user(size_t count) {
#if defined(__i386__)
    return alloc_frames_high(count);
#else
    return alloc_frames(count);
#endif
}

void free_frames(void *ptr, size_t size) {
    unsigned long irq_flags = spin_lock_irqsave(&pmm.lock);

    if (!pmm.refs_ready) {
        bitmap_alloc_free(&pmm.frames, ptr, size);
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        return;
    }

    if (!ptr || !size) {
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        return;
    }

    size_t start = _pmm_block_index(ptr);

    for (size_t i = 0; i < size; i++) {
        size_t index = start + i;

        if (index >= pmm.ref_count) {
            continue;
        }

        if (pmm.refs[index] > 0) {
            pmm.refs[index]--;
        }

        if (!pmm.refs[index]) {
            bitmap_alloc_free(&pmm.frames, bitmap_alloc_to_ptr(&pmm.frames, index), 1);
        }
    }

#ifdef MMU_DEBUG
    log_debug("PMM freed %zu frames paddr=%#" PRIx64, size, (u64)(uintptr_t)ptr);
#endif

    spin_unlock_irqrestore(&pmm.lock, irq_flags);
}

void pmm_ref_hold(void *ptr, size_t blocks) {
    unsigned long irq_flags = spin_lock_irqsave(&pmm.lock);

    if (!pmm.refs_ready || !ptr || !blocks) {
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        return;
    }

    size_t start = _pmm_block_index(ptr);

    for (size_t i = 0; i < blocks; i++) {
        size_t index = start + i;

        if (index < pmm.ref_count && pmm.refs[index] < UINT16_MAX) {
            pmm.refs[index]++;
        }
    }

    spin_unlock_irqrestore(&pmm.lock, irq_flags);
}

u16 pmm_refcount(void *ptr) {
    unsigned long irq_flags = spin_lock_irqsave(&pmm.lock);

    if (!pmm.refs_ready || !ptr) {
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        return 1;
    }

    size_t index = _pmm_block_index(ptr);

    if (index >= pmm.ref_count) {
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        return 1;
    }

    u16 refs = pmm.refs[index];
    spin_unlock_irqrestore(&pmm.lock, irq_flags);
    return refs;
}


void reclaim_boot_map(e820_map_t *mmap) {
    for (size_t i = 0; i < mmap->count; i++) {
        e820_entry_t *current = &mmap->entries[i];

        if (current->type == E820_ALLOC) {
            current->type = E820_AVAILABLE;
        }
    }

    clean_mmap(mmap);

#if defined(__x86_64__)
    u64 root = read_cr3();
    page_t *root_vaddr = (page_t *)(root + LINEAR_MAP_OFFSET_64);

    // the higher half contains kernel mappings
    memset(root_vaddr, 0, 256 * sizeof(page_t));

    // flush the TLB
    write_cr3(root);
#endif
}
