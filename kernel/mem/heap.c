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

#include "arch/panic.h"
#include "mem/physical.h"

// FIXME: baaaaaad. A bitmap is far from optimal for this

// We should have more than one heap
static bitmap_alloc heap;


void heap_init() {
    usize free_pages = get_free_mem() / PAGE_4KIB;

    usize heap_pages = min(KERNEL_HEAP_PAGES, free_pages);
    usize heap_size = heap_pages * PAGE_4KIB;

    void* heap_start = (void*)ID_MAPPED_VADDR(alloc_frames(min(KERNEL_HEAP_PAGES, free_pages)));

    if (!bitmap_alloc_init(&heap, heap_start, heap_size, KERNEL_HEAP_BLOCK))
        panic("Failed to initialize kernel heap!");
}


void* kmalloc(usize size) {
    if (!size) {
#ifdef KMALLOC_DEBUG
        log_warn("[KMALLOC_DEBUG] malloc: bytes = 0");
#endif

        return NULL;
    }

    usize blocks = DIV_ROUND_UP(size, KERNEL_HEAP_BLOCK);
    void* space = bitmap_alloc_blocks(&heap, blocks + 1);

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
    usize size = header->size * KERNEL_HEAP_BLOCK;

#ifdef KMALLOC_DEBUG
    log_debug("[KMALLOC_DEBUG] free: bytes = %zd, ptr = %#lx", size, (u64)ptr);
#endif

    assert(header->magic == KERNEL_HEAP_MAGIC);
    assert(header->size != 0);

    bitmap_alloc_free(&heap, header, size + sizeof(kheap_header));
}


static global_alloc galloc = {0};
global_alloc* _global_allocator = NULL;

void galloc_init() {
    _global_allocator = &galloc;

    _global_allocator->malloc = kmalloc;
    _global_allocator->calloc = kcalloc;

    _global_allocator->free = kfree;
}
