#include "heap.h"

#include <alloc/bitmap.h>
#include <alloc/global.h>
#include <base/addr.h>
#include <base/macros.h>
#include <boot/proto.h>
#include <log/log.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <x86/paging.h>

#include "base/types.h"
#include "mem/physical.h"
#include "sys/panic.h"

#define HEAP_MIN (KERNEL_HEAP_PAGES / 2)
#define HEAP_MAX (KERNEL_HEAP_PAGES * 16)

// FIXME: baaaaaad. A bitmap is far from optimal for this

// We should have more than one heap
static bitmap_alloc heap = {0};


void heap_init() {
    usize free_pages = get_free_mem() / PAGE_4KIB;

    // Aim to take ~10% of the memory for the kernel heap with some reasonable limits
    usize min_heap = min(free_pages, HEAP_MIN);
    usize max_heap = HEAP_MAX;

    usize heap_pages = clamp(free_pages / 10, min_heap, max_heap);
    usize heap_size = heap_pages * PAGE_4KIB;

    void* paddr = alloc_frames(heap_pages);

    assert(paddr);

    void* heap_start = (void*)ID_MAPPED_VADDR(paddr);

    if (!bitmap_alloc_init(&heap, heap_start, heap_size, KERNEL_HEAP_BLOCK_SIZE))
        panic("Failed to initialize kernel heap!");
}


void* kmalloc(usize size) {
    if (!size) {
#ifdef KMALLOC_DEBUG
        log_warn("[KMALLOC_DEBUG] malloc: bytes = 0");
#endif

        return NULL;
    }

    usize header_blocks = DIV_ROUND_UP(sizeof(kheap_header), KERNEL_HEAP_BLOCK_SIZE);
    usize blocks = DIV_ROUND_UP(size, KERNEL_HEAP_BLOCK_SIZE);

    void* space = bitmap_alloc_reserve(&heap, blocks + header_blocks);

    // No more memory left. Since were in kernel land we treat this as a fatal error
    // TODO: attempt to allocate more heaps
    assert(space != NULL);

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

void* kcalloc(usize size) {
    void* ret = kmalloc(size);

    if (ret)
        memset(ret, 0, size);

    return ret;
}

void kfree(void* ptr) {
    if (!ptr) {
#ifdef KMALLOC_DEBUG
        log_warn("[KMALLOC_DEBUG] free: ptr = NULL");
#endif
        return;
    }

    kheap_header* header = ptr - sizeof(kheap_header);

    assert(header->magic == KERNEL_HEAP_MAGIC);
    assert(header->size != 0);

    usize header_blocks = DIV_ROUND_UP(sizeof(kheap_header), KERNEL_HEAP_BLOCK_SIZE);
    usize blocks = header->size + header_blocks;

    bitmap_alloc_free(&heap, header, blocks);

#ifdef KMALLOC_DEBUG
    usize size = header->size * KERNEL_HEAP_BLOCK_SIZE;
    log_debug("[KMALLOC_DEBUG] free: bytes = %zd, ptr = %#lx", size, (u64)ptr);
#endif
}


static global_alloc galloc = {0};
global_alloc* _global_allocator = NULL;

void galloc_init() {
    _global_allocator = &galloc;

    _global_allocator->malloc = kmalloc;
    _global_allocator->calloc = kcalloc;

    _global_allocator->free = kfree;
}
