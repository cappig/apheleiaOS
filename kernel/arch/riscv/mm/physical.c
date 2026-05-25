#include "physical.h"

#include <alloc/bitmap.h>
#include <arch/paging.h>
#include <base/macros.h>
#include <data/bitmap.h>
#include <limits.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/panic.h>

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

void pmm_init(u64 mem_base, u64 mem_size, u64 reserved_end) {
    mem_base = ALIGN(mem_base, PAGE_4KIB);
    u64 mem_top = ALIGN_DOWN(mem_base + mem_size, PAGE_4KIB);
    reserved_end = ALIGN(reserved_end, PAGE_4KIB);

    if (mem_top <= mem_base || reserved_end >= mem_top) {
        panic("invalid RISC-V memory map");
    }

    memset(&pmm.frames, 0, sizeof(pmm.frames));
    pmm.frames.chuck_start = (void *)(uintptr_t)mem_base;
    pmm.frames.chunk_size = (size_t)(mem_top - mem_base);
    pmm.frames.block_size = PAGE_4KIB;
    pmm.frames.block_count = pmm.frames.chunk_size / pmm.frames.block_size;
    pmm.frames.word_count = DIV_ROUND_UP(pmm.frames.block_count, BITMAP_WORD_SIZE);

    size_t bitmap_bytes = DIV_ROUND_UP(pmm.frames.block_count, 8);
    size_t bitmap_size = ALIGN(bitmap_bytes, PAGE_4KIB);
    u64 bitmap_addr = reserved_end;
    u64 alloc_base = bitmap_addr + bitmap_size;

    if (alloc_base > mem_top) {
        panic("RISC-V PMM bitmap does not fit in RAM");
    }

    pmm.frames.bitmap = (bitmap_word_t *)(uintptr_t)bitmap_addr;
    memset(pmm.frames.bitmap, 0xff, bitmap_size);

    size_t start_block = 0;
    size_t free_blocks = 0;

    if (alloc_base > mem_base) {
        start_block = (size_t)((alloc_base - mem_base) / PAGE_4KIB);
    }

    if (mem_top > alloc_base) {
        free_blocks = (size_t)((mem_top - alloc_base) / PAGE_4KIB);
    }

    if (free_blocks) {
        bitmap_clear_region(pmm.frames.bitmap, start_block, free_blocks);
    }

    pmm.frames.next_fit_block = start_block;
    pmm.frames.free_blocks = free_blocks;
    pmm.frames.usable_blocks = free_blocks;

    log_debug(
        "RISC-V PMM ready: base=%#llx size=%zu KiB free=%zu KiB",
        (unsigned long long)mem_base,
        (size_t)((mem_top - mem_base) / 1024),
        pmm_free_mem() / 1024
    );
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
        log_warn("failed to allocate RISC-V PMM refcount table");
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
    if (!ret) {
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        panic("RISC-V PMM exhausted");
    }

    _pmm_ref_set_range(ret, count, 1);
    spin_unlock_irqrestore(&pmm.lock, irq_flags);
    return ret;
}

void *alloc_frames_high(size_t count) {
    assert(count);

    unsigned long irq_flags = spin_lock_irqsave(&pmm.lock);
    void *ret = bitmap_alloc_reserve_high(&pmm.frames, count);
    if (!ret) {
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        panic("RISC-V PMM exhausted");
    }

    _pmm_ref_set_range(ret, count, 1);
    spin_unlock_irqrestore(&pmm.lock, irq_flags);
    return ret;
}

void *alloc_frames_user(size_t count) {
    return alloc_frames_high(count);
}

void free_frames(void *ptr, size_t count) {
    unsigned long irq_flags = spin_lock_irqsave(&pmm.lock);

    if (!pmm.refs_ready) {
        bitmap_alloc_free(&pmm.frames, ptr, count);
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        return;
    }

    if (!ptr || !count) {
        spin_unlock_irqrestore(&pmm.lock, irq_flags);
        return;
    }

    size_t start = _pmm_block_index(ptr);
    for (size_t i = 0; i < count; i++) {
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
