#include "heap.h"

#include <alloc/bitmap.h>
#include <alloc/global.h>
#include <base/addr.h>
#include <base/macros.h>
#include <boot/proto.h>
#include <stddef.h>
#include <string.h>
#include <x86/paging.h>

#include "physical.h"
#include "video/tty.h"

// FIXME: baaaaaad. A bitmap is far from optimal for this

// We should have more than one heap
static bitmap_alloc heap = {0};


void heap_init() {
    usize heap_size = KERNEL_HEAP_PAGES * PAGE_4KIB;
    void* heap_start = (void*)ID_MAPPED_VADDR(alloc_frames(KERNEL_HEAP_PAGES));

    if (!bitmap_alloc_init(&heap, heap_start, heap_size, KERNEL_HEAP_BLOCK))
        panic("Falied to iniralize kernel heap!");
}


void* kmalloc(usize size) {
    if (!size)
        return NULL;

    usize blocks = DIV_ROUND_UP(size, KERNEL_HEAP_BLOCK);

    void* space = bitmap_alloc_blocks(&heap, blocks + 1);
    if (!space)
        panic("Kernel heap out of memory!");

    // Write the header
    kheap_header* header = space;
    header->magic = KERNEL_HEAP_MAGIC;
    header->size = blocks;

    return space + sizeof(kheap_header);
}

void* kcalloc(usize size) {
    void* ret = kmalloc(size);

    memset(ret, 0, size);

    return ret;
}

void kfree(void* ptr) {
    kheap_header* header = ptr - sizeof(kheap_header);

    // FIXME: This should really not panic
    if (header->magic != KERNEL_HEAP_MAGIC || !header->size)
        panic("Attempted to free unallocated memory!");

    bitmap_alloc_free(&heap, header, (header->size + 1) * KERNEL_HEAP_BLOCK);
}


static global_alloc galloc = {0};
global_alloc* _global_allocator = NULL;

void galloc_init() {
    //_global_allocator = kcalloc(sizeof(global_alloc));
    // if (!_global_allocator) panic("Failed to init galloc");

    _global_allocator = &galloc;

    _global_allocator->malloc = kmalloc;
    _global_allocator->calloc = kcalloc;

    _global_allocator->free = kfree;
}
