#include "heap.h"

#include <alloc/bitmap.h>
#include <arch/paging.h>
#include <base/macros.h>
#include <inttypes.h>
#include <log/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/panic.h>

#include "physical.h"

#define HEAP_MIN        (KERNEL_HEAP_PAGES / 2)
#define HEAP_MAX        (KERNEL_HEAP_PAGES * 16)
#define HEAP_MAX_ARENAS 16

typedef struct {
    bitmap_allocator_t alloc;
    size_t pages;
    bool used;
} heap_arena_t;

static heap_arena_t heap_arenas[HEAP_MAX_ARENAS] = {0};
static size_t heap_arena_count = 0;
static spinlock_t heap_lock = SPINLOCK_INIT;

static size_t _usable_blocks_for_pages(size_t pages) {
    if (!pages) {
        return 0;
    }

    size_t chunk_size = pages * PAGE_4KIB;
    size_t block_count = chunk_size / KERNEL_HEAP_BLOCK_SIZE;
    size_t bitmap_bytes = DIV_ROUND_UP(block_count, 8);
    size_t bitmap_blocks = DIV_ROUND_UP(bitmap_bytes, KERNEL_HEAP_BLOCK_SIZE);

    if (bitmap_blocks >= block_count) {
        return 0;
    }

    return block_count - bitmap_blocks;
}

static size_t _pages_for_blocks(size_t blocks) {
    if (!blocks) {
        return 0;
    }

    size_t pages = DIV_ROUND_UP(blocks * KERNEL_HEAP_BLOCK_SIZE, PAGE_4KIB);
    if (!pages) {
        pages = 1;
    }

    while (_usable_blocks_for_pages(pages) < blocks) {
        if (pages == SIZE_MAX) {
            return 0;
        }
        pages++;
    }

    return pages;
}

static bool _add_arena(size_t pages) {
    if (!pages || heap_arena_count >= HEAP_MAX_ARENAS) {
        return false;
    }

    size_t free_pages = pmm_free_mem() / PAGE_4KIB;
    if (pages > free_pages) {
        return false;
    }

    size_t chunk_size = pages * PAGE_4KIB;
    void *start = alloc_frames(pages);
    if (!start) {
        return false;
    }

    heap_arena_t *arena = &heap_arenas[heap_arena_count];
    if (!bitmap_alloc_init(&arena->alloc, start, chunk_size, KERNEL_HEAP_BLOCK_SIZE)) {
        free_frames(start, pages);
        return false;
    }

    arena->pages = pages;
    arena->used = true;
    heap_arena_count++;
    return true;
}

static bool _grow(size_t min_blocks) {
    if (!min_blocks || heap_arena_count >= HEAP_MAX_ARENAS) {
        return false;
    }

    size_t min_pages = _pages_for_blocks(min_blocks);
    if (!min_pages) {
        return false;
    }

    size_t grow_pages = KERNEL_HEAP_PAGES;
    if (heap_arena_count) {
        grow_pages = heap_arenas[heap_arena_count - 1].pages;
    }

    if (grow_pages < min_pages) {
        grow_pages = min_pages;
    }

    size_t free_pages = pmm_free_mem() / PAGE_4KIB;
    if (grow_pages > free_pages) {
        grow_pages = free_pages;
    }

    if (grow_pages < min_pages) {
        return false;
    }

    return _add_arena(grow_pages);
}

static heap_arena_t *_find_arena_by_ptr(const void *ptr) {
    uintptr_t addr = (uintptr_t)ptr;

    for (size_t i = 0; i < heap_arena_count; i++) {
        heap_arena_t *arena = &heap_arenas[i];
        if (!arena->used) {
            continue;
        }

        uintptr_t start = (uintptr_t)arena->alloc.chuck_start;
        uintptr_t end = start + arena->alloc.chunk_size;
        if (addr >= start && addr < end) {
            return arena;
        }
    }

    return NULL;
}

void heap_init(void) {
    unsigned long irq_flags = spin_lock_irqsave(&heap_lock);

    size_t free_pages = pmm_free_mem() / PAGE_4KIB;
    size_t min_heap = min(free_pages, (size_t)HEAP_MIN);
    size_t heap_pages = clamp(free_pages / 3, min_heap, (size_t)HEAP_MAX);

    if (!_add_arena(heap_pages)) {
        spin_unlock_irqrestore(&heap_lock, irq_flags);
        panic("failed to initialize RISC-V kernel heap");
    }

    spin_unlock_irqrestore(&heap_lock, irq_flags);
}

static void *_kmalloc(size_t size) {
    if (!size) {
        return NULL;
    }

    unsigned long irq_flags = spin_lock_irqsave(&heap_lock);

    size_t header_blocks =
        DIV_ROUND_UP(sizeof(kheap_header_t), KERNEL_HEAP_BLOCK_SIZE);
    size_t blocks = DIV_ROUND_UP(size, KERNEL_HEAP_BLOCK_SIZE);
    size_t total_blocks = blocks + header_blocks;

    void *space = NULL;
    for (size_t i = 0; i < heap_arena_count; i++) {
        heap_arena_t *arena = &heap_arenas[i];
        if (!arena->used) {
            continue;
        }

        space = bitmap_alloc_reserve(&arena->alloc, total_blocks);
        if (space) {
            break;
        }
    }

    if (!space) {
        if (!_grow(total_blocks)) {
            spin_unlock_irqrestore(&heap_lock, irq_flags);
            panic("kmalloc out of memory (%zu bytes)", size);
        }

        heap_arena_t *arena = &heap_arenas[heap_arena_count - 1];
        space = bitmap_alloc_reserve(&arena->alloc, total_blocks);
    }

    if (!space) {
        spin_unlock_irqrestore(&heap_lock, irq_flags);
        panic("kmalloc failed after grow (%zu bytes)", size);
    }

    kheap_header_t *header = space;
    header->magic = KERNEL_HEAP_MAGIC;
    header->size = blocks;

    void *ret = (u8 *)space + sizeof(*header);
    spin_unlock_irqrestore(&heap_lock, irq_flags);
    return ret;
}

static void _kfree(void *ptr) {
    if (!ptr) {
        return;
    }

    unsigned long irq_flags = spin_lock_irqsave(&heap_lock);

    kheap_header_t *header = (kheap_header_t *)((u8 *)ptr - sizeof(*header));
    if (header->magic != KERNEL_HEAP_MAGIC) {
        spin_unlock_irqrestore(&heap_lock, irq_flags);
        panic("invalid RISC-V heap header");
    }

    size_t header_blocks =
        DIV_ROUND_UP(sizeof(kheap_header_t), KERNEL_HEAP_BLOCK_SIZE);
    size_t blocks = header->size + header_blocks;
    heap_arena_t *arena = _find_arena_by_ptr(header);
    if (!arena) {
        spin_unlock_irqrestore(&heap_lock, irq_flags);
        panic("kfree pointer is outside the RISC-V heap");
    }

    bitmap_alloc_free(&arena->alloc, header, blocks);
    spin_unlock_irqrestore(&heap_lock, irq_flags);
}

void arch_init_alloc(void) {
    libc_alloc_ops_t ops = {
        .malloc_fn = _kmalloc,
        .free_fn = _kfree,
    };

    __libc_init_alloc(&ops);
}
