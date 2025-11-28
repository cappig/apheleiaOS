#pragma once

#include <base/types.h>
#include <data/bitmap.h>

#define BITMAP_FREE 0
#define BITMAP_USED 1

typedef struct {
    void* chuck_start;
    size_t chunk_size;

    size_t block_size;
    size_t block_count;
    size_t free_blocks;

    size_t word_count;
    bitmap_word_t* bitmap;
} bitmap_allocator_t;


inline size_t bitmap_alloc_to_block(bitmap_allocator_t* alloc, void* ptr) {
    return (ptr - alloc->chuck_start) / alloc->block_size;
}

inline bitmap_word_t* bitmap_alloc_to_ptr(bitmap_allocator_t* alloc, size_t block) {
    return alloc->chuck_start + block * alloc->block_size;
}

bool bitmap_alloc_init(
    bitmap_allocator_t* alloc,
    void* chunk_start,
    size_t chunk_size,
    size_t block_size
);

void* bitmap_alloc_reserve(bitmap_allocator_t* alloc, size_t blocks);
bool bitmap_alloc_free(bitmap_allocator_t* alloc, void* ptr, size_t blocks);
