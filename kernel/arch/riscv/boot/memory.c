#include <base/types.h>
#include <limits.h>
#include <string.h>

extern char __heap_start;
extern char __heap_end;

typedef struct {
    uintptr_t cursor;
    uintptr_t limit;
} boot_heap_t;

static boot_heap_t boot_heap;

static uintptr_t align_up(uintptr_t value, uintptr_t align) {
    return (value + align - 1) & ~(align - 1);
}

void boot_heap_init(uintptr_t start, uintptr_t end) {
    boot_heap.cursor = align_up(start, 16);
    boot_heap.limit = end & ~(uintptr_t)0xf;
}

void *boot_alloc_aligned(size_t size, size_t align, bool zero) {
    if (!size) {
        return NULL;
    }

    if (!boot_heap.cursor) {
        boot_heap_init((uintptr_t)&__heap_start, (uintptr_t)&__heap_end);
    }

    if (!align) {
        align = 16;
    }

    uintptr_t cursor = align_up(boot_heap.cursor, align);
    size = align_up(size, 16);

    if (!boot_heap.limit || cursor + size < cursor || cursor + size > boot_heap.limit) {
        return NULL;
    }

    void *ptr = (void *)cursor;
    boot_heap.cursor = cursor + size;

    if (zero) {
        memset(ptr, 0, size);
    }

    return ptr;
}

void *malloc(size_t size) {
    return boot_alloc_aligned(size, 16, false);
}

void *calloc(size_t count, size_t size) {
    if (!count || !size) {
        return NULL;
    }

    if (count > SIZE_MAX / size) {
        return NULL;
    }

    size_t total = count * size;
    return boot_alloc_aligned(total, 16, true);
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }

    if (!size) {
        return NULL;
    }

    return boot_alloc_aligned(size, 16, false);
}

void free(void *ptr) {
    (void)ptr;
}
