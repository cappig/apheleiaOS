#include "heap.h"

#include <arch/arch.h>
#include <arch/mm.h>
#include <arch/paging.h>
#include <alloc/bitmap.h>
#include <base/macros.h>
#include <base/types.h>
#include <inttypes.h>
#include <log/log.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/lock.h>
#include <sys/panic.h>

#if defined(__x86_64__)
#include "x86/paging64.h"
#else
#include "x86/paging32.h"
#include "x86/boot.h"
#endif

#include "physical.h"

#define HEAP_MIN        (KERNEL_HEAP_PAGES / 2)
#define HEAP_MAX        (KERNEL_HEAP_PAGES * 16)
#define HEAP_MAX_ARENAS 16

// FIXME: baaaaaad. A bitmap is far from optimal for this

typedef struct {
    bitmap_allocator_t alloc;
    size_t pages;
    bool used;
} heap_arena_t;

static heap_arena_t heap_arenas[HEAP_MAX_ARENAS] = {0};
static size_t heap_arena_count = 0;
static spinlock_t heap_lock = SPINLOCK_INIT;
#if defined(__i386__)
static uintptr_t heap_vaddr_next = 0;
static uintptr_t heap_vaddr_limit = 0;
#endif

#if defined(__x86_64__)
static uintptr_t _linear_offset(void) {
    return LINEAR_MAP_OFFSET_64;
}
#endif

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

    void *paddr = alloc_frames(pages);
    if (!paddr) {
        return false;
    }

#if defined(__i386__)
    void *root = arch_vm_root(arch_vm_kernel());
    if (!root) {
        free_frames(paddr, pages);
        return false;
    }

    if (!heap_vaddr_limit) {
        extern char __kernel_end;
        heap_vaddr_next = ALIGN((uintptr_t)&__kernel_end, PAGE_4KIB);
        heap_vaddr_limit = KSTACK_REGION_BASE_32;
    }

    size_t size = pages * PAGE_4KIB;
    uintptr_t vaddr = heap_vaddr_next;

    if (!heap_vaddr_limit || vaddr + size < vaddr ||
        vaddr + size > heap_vaddr_limit) {
        free_frames(paddr, pages);
        return false;
    }

    arch_map_region(root, pages, vaddr, (uintptr_t)paddr, PT_WRITE);
    heap_vaddr_next = vaddr + size;

    void *start = (void *)vaddr;
#else
    void *start = (void *)((uintptr_t)paddr + _linear_offset());
#endif

    heap_arena_t *arena = &heap_arenas[heap_arena_count];

    bool alloc_inited = bitmap_alloc_init(
        &arena->alloc,
        start,
        chunk_size,
        KERNEL_HEAP_BLOCK_SIZE
    );

    if (!alloc_inited) {
        free_frames(paddr, pages);
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

    if (!_add_arena(grow_pages)) {
        return false;
    }

    return true;
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


void heap_init() {
    log_debug("initializing kernel heap");
    unsigned long irq_flags = spin_lock_irqsave(&heap_lock);
    size_t free_pages = pmm_free_mem() / PAGE_4KIB;

    // Aim to take ~33% of the memory for the kernel heap
    size_t min_heap = min(free_pages, HEAP_MIN);
    size_t max_heap = HEAP_MAX;

    size_t heap_pages = clamp(free_pages / 3, min_heap, max_heap);

    if (!_add_arena(heap_pages)) {
        spin_unlock_irqrestore(&heap_lock, irq_flags);
        panic("Failed to initialize kernel heap");
    }

    spin_unlock_irqrestore(&heap_lock, irq_flags);
}


static void *_kmalloc(size_t size) {
    if (!size) {
#ifdef KMALLOC_DEBUG
        log_warn("[KMALLOC_DEBUG] malloc bytes=0");
#endif
        return NULL;
    }

    unsigned long irq_flags = spin_lock_irqsave(&heap_lock);
    size_t header_blocks =
        DIV_ROUND_UP(sizeof(kheap_header), KERNEL_HEAP_BLOCK_SIZE);

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
            panic("kmalloc out of heap memory (requested=%zu bytes)", size);
        }

        heap_arena_t *arena = &heap_arenas[heap_arena_count - 1];
        space = bitmap_alloc_reserve(&arena->alloc, total_blocks);
    }

    if (!space) {
        panic(
            "kmalloc out of heap memory after _grow (requested=%zu bytes)",
            size
        );
    }

    // Write the header
    kheap_header *header = space;

    header->magic = KERNEL_HEAP_MAGIC;
    header->size = blocks;

    void *ret = (u8 *)space + sizeof(kheap_header);

#ifdef KMALLOC_DEBUG
    log_debug(
        "[KMALLOC_DEBUG] malloc bytes=%zd ptr=%#" PRIx64,
        size,
        (u64)(uintptr_t)ret
    );
#endif

    spin_unlock_irqrestore(&heap_lock, irq_flags);
    return ret;
}

static void _kfree(void *ptr) {
    if (!ptr) {
#ifdef KMALLOC_DEBUG
        log_warn("[KMALLOC_DEBUG] free ptr is NULL");
#endif
        return;
    }

    unsigned long irq_flags = spin_lock_irqsave(&heap_lock);
    kheap_header *header = (kheap_header *)((u8 *)ptr - sizeof(kheap_header));

    if (header->magic != KERNEL_HEAP_MAGIC) {
        panic("kfree invalid heap header");
    }

    size_t header_blocks =
        DIV_ROUND_UP(sizeof(kheap_header), KERNEL_HEAP_BLOCK_SIZE);

    size_t blocks = header->size + header_blocks;
    heap_arena_t *arena = _find_arena_by_ptr(header);

    if (!arena) {
        panic("kfree pointer does not belong to any heap arena");
    }

    bitmap_alloc_free(&arena->alloc, header, blocks);

#ifdef KMALLOC_DEBUG
    size_t size = header->size * KERNEL_HEAP_BLOCK_SIZE;
    log_debug(
        "[KMALLOC_DEBUG] free bytes=%zd ptr=%#" PRIx64,
        size,
        (u64)(uintptr_t)ptr
    );
#endif

    spin_unlock_irqrestore(&heap_lock, irq_flags);
}


void arch_init_alloc() {
    log_debug("initializing kernel malloc");

    libc_alloc_ops_t ops = {
        .malloc_fn = _kmalloc,
        .free_fn = _kfree,
    };
    __libc_init_alloc(&ops);

    // log_debug("malloc ready");
}
