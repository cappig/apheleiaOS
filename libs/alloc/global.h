#pragma once

#include <stddef.h>

typedef void* (*alloc_fn)(size_t size);
typedef void* (*alloc_aligned_fn)(size_t alignment, size_t size);

typedef void (*free_fn)(void* ptr);
typedef void (*free_sized_fn)(void* ptr, size_t size);

// This structure is kept in global memory
// It allows internal libs to make dynamic allocations
typedef struct {
    alloc_fn malloc;
    alloc_fn calloc;
    alloc_aligned_fn malloc_aligned;

    free_fn free;
} global_alloc;

// This global variable MUST be initialized by the "user" (bootloader/kernel)
// Calling lib functions with this struct uninitialized will lead to nullptr dereferencing
extern global_alloc* _global_allocator;

// Wrappers to make our code look pretty :^)
inline void* gmalloc(size_t size) {
    return _global_allocator->malloc(size);
}

inline void* gcalloc(size_t size) {
    return _global_allocator->calloc(size);
}

inline void gfree(void* ptr) {
    _global_allocator->free(ptr);
}
