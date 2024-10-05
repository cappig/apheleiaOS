#include "heap.h"

#include <alloc/bitmap.h>
#include <alloc/global.h>
#include <base/addr.h>
#include <base/macros.h>
#include <boot/proto.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <x86/paging.h>

#include "arch/panic.h"
#include "log/log.h"
#include "physical.h"

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
    if (!size)
        return NULL;

    usize blocks = DIV_ROUND_UP(size, KERNEL_HEAP_BLOCK);

    // FIXME: don't just panic. Attempt to allocate more heaps
    void* space = bitmap_alloc_blocks(&heap, blocks + 1);
    if (!space) {
        log_fatal("Attempted to allocate %zd blocks!", blocks);
        panic("Kernel heap out of memory: %zd free blocks left!", heap.free_blocks);
    }

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
    if (!ret)
        return NULL;

    memset(ret, 0, size);

    return ret;
}

void kfree(void* ptr) {
    kheap_header* header = ptr - sizeof(kheap_header);

    usize size = header->size * KERNEL_HEAP_BLOCK;

#ifdef KMALLOC_DEBUG
    log_debug("[KMALLOC_DEBUG] free: bytes = %zd, ptr = %#lx", size, (u64)ptr);
#endif

    // FIXME: This should really not panic
    if (header->magic != KERNEL_HEAP_MAGIC || !header->size)
        panic("Attempted to free unallocated memory!");

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
