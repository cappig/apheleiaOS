#include "heap.h"

#include <alloc/bitmap.h>
#include <base/macros.h>
#include <base/types.h>
#include <log/log.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__)
#include "x86/paging64.h"
#else
#include "x86/paging32.h"
#endif

#include "physical.h"

#define HEAP_MIN (KERNEL_HEAP_PAGES / 2)
#define HEAP_MAX (KERNEL_HEAP_PAGES * 16)

// FIXME: baaaaaad. A bitmap is far from optimal for this

// TODO: have more than one heap, if run out of heap space allocate a new heap
static bitmap_allocator_t heap = {0};


void heap_init() {
    size_t free_pages = pmm_free_mem() / PAGE_4KIB;

    // Aim to take ~10% of the memory for the kernel heap with some reasonable limits
    size_t min_heap = min(free_pages, HEAP_MIN);
    size_t max_heap = HEAP_MAX;

    size_t heap_pages = clamp(free_pages / 10, min_heap, max_heap);
    size_t heap_size = heap_pages * PAGE_4KIB;

    void* paddr = alloc_frames(heap_pages);

    // assert(paddr);


#if defined(__x86_64__)
    uintptr_t heap_offset = LINEAR_MAP_OFFSET_64;
#else
    uintptr_t heap_offset = LINEAR_MAP_OFFSET_32;
#endif

    void* heap_start = (void*)((uintptr_t)paddr + heap_offset);

    bitmap_alloc_init(&heap, heap_start, heap_size, KERNEL_HEAP_BLOCK_SIZE);

    // if (!bitmap_alloc_init(&heap, heap_start, heap_size, KERNEL_HEAP_BLOCK_SIZE))
    //     panic("Failed to initialize kernel heap!");
}


static void* _kmalloc(size_t size) {
    if (!size) {
#ifdef KMALLOC_DEBUG
        log_warn("[KMALLOC_DEBUG] malloc: bytes = 0");
#endif
        return NULL;
    }

    size_t header_blocks = DIV_ROUND_UP(sizeof(kheap_header), KERNEL_HEAP_BLOCK_SIZE);
    size_t blocks = DIV_ROUND_UP(size, KERNEL_HEAP_BLOCK_SIZE);

    void* space = bitmap_alloc_reserve(&heap, blocks + header_blocks);

    // No more memory left. Since were in kernel land we treat this as a fatal error
    // TODO: attempt to allocate more heaps
    // assert(space != NULL);

    // Write the header
    kheap_header* header = space;

    header->magic = KERNEL_HEAP_MAGIC;
    header->size = blocks;

    void* ret = space + sizeof(kheap_header);

#ifdef KMALLOC_DEBUG
    log_debug("[KMALLOC_DEBUG] malloc: bytes = %zd, ptr = %#lx", size, (u64)ret);
#endif

    return ret;
}

static void _kfree(void* ptr) {
    if (!ptr) {
#ifdef KMALLOC_DEBUG
        log_warn("[KMALLOC_DEBUG] free: ptr = NULL");
#endif
        return;
    }

    kheap_header* header = ptr - sizeof(kheap_header);

    // assert(header->magic == KERNEL_HEAP_MAGIC);
    // assert(header->size != 0);

    size_t header_blocks = DIV_ROUND_UP(sizeof(kheap_header), KERNEL_HEAP_BLOCK_SIZE);
    size_t blocks = header->size + header_blocks;

    bitmap_alloc_free(&heap, header, blocks);

#ifdef KMALLOC_DEBUG
    size_t size = header->size * KERNEL_HEAP_BLOCK_SIZE;
    log_debug("[KMALLOC_DEBUG] free: bytes = %zd, ptr = %#lx", size, (u64)ptr);
#endif
}


static struct _external_alloc external_alloc = {0};
struct _external_alloc* _external_alloc = NULL;

void init_malloc() {
    _external_alloc = &external_alloc;

    _external_alloc->malloc = _kmalloc;
    _external_alloc->free = _kfree;
}
