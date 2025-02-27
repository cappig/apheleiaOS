#pragma once

#include <base/types.h>
#include <data/bitmap.h>
#include <x86/e820.h>

#define BITMAP_FREE 0
#define BITMAP_USED 1

#define ALLOC_OUT_OF_BLOCKS (-1)

typedef struct {
    void* chuck_start;
    usize chunk_size;

    usize block_size;
    usize block_count;
    usize free_blocks;

    usize word_count;
    bitmap_word* bitmap;
} bitmap_alloc;


bool bitmap_alloc_init(bitmap_alloc* alloc, void* chunk_start, usize chunk_size, usize block_size);
bool bitmap_alloc_init_mmap(bitmap_alloc* alloc, e820_map* mmap, usize block_size);

void* bitmap_alloc_reserve(bitmap_alloc* alloc, usize blocks);
bool bitmap_alloc_free(bitmap_alloc* alloc, void* ptr, usize blocks);
