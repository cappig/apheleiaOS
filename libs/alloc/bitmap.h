#pragma once

#include <base/types.h>
#include <data/bitmap.h>

#define BITMAP_FREE 0
#define BITMAP_USED 1

#define ALLOC_OUT_OF_BLOCKS ((usize)-1)

typedef struct {
    void* chuck_start;
    usize chunk_size;

    usize block_size;
    usize block_count;

    usize word_count;
    bitmap_word* bitmap;
} bitmap_alloc;


bool bitmap_alloc_init(bitmap_alloc* alloc, void* chunk_start, usize chunk_size, usize block_size);

void* bitmap_alloc_malloc(bitmap_alloc* alloc, usize bytes);
