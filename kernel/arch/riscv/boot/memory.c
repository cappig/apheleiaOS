#include <base/types.h>
#include <limits.h>
#include <string.h>

extern char __heap_start;
extern char __heap_end;

static uintptr_t heap_cursor;
static uintptr_t heap_limit;

static uintptr_t align_up(uintptr_t value, uintptr_t align) {
    return (value + align - 1) & ~(align - 1);
}

void boot_heap_init(uintptr_t start, uintptr_t end) {
    heap_cursor = align_up(start, 16);
    heap_limit = end & ~(uintptr_t)0xf;
}

void *boot_alloc_aligned(size_t size, size_t align, bool zero) {
    if (!size) {
        return NULL;
    }

    if (!heap_cursor) {
        boot_heap_init((uintptr_t)&__heap_start, (uintptr_t)&__heap_end);
    }

    if (!align) {
        align = 16;
    }

    uintptr_t cursor = align_up(heap_cursor, align);
    size = align_up(size, 16);

    if (!heap_limit || cursor + size < cursor || cursor + size > heap_limit) {
        return NULL;
    }

    void *ptr = (void *)cursor;
    heap_cursor = cursor + size;

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
