#include "bitmap.h"

#include <base/macros.h>
#include <base/types.h>
#include <data/bitmap.h>
#include <string.h>

static inline usize _to_block(usize block_size, uptr mem_base, bitmap_word* ptr) {
    return ((u64)ptr - mem_base) / block_size;
}

static inline bitmap_word* _to_ptr(usize block_size, uptr mem_base, usize block) {
    return (void*)(mem_base + block * block_size);
}


bool bitmap_alloc_init(bitmap_alloc* alloc, void* chunk_start, usize chunk_size, usize block_size) {
    alloc->chuck_start = chunk_start;
    alloc->chunk_size = chunk_size;

    alloc->block_size = block_size;
    alloc->block_count = chunk_size / block_size;
    alloc->word_count = alloc->block_count / BITMAP_WORD_SIZE;

    usize bitmap_size = DIV_ROUND_UP(alloc->block_count, BITMAP_WORD_SIZE);

    if (chunk_size <= bitmap_size)
        return false;

    // Place the bitmap at the start of the chunk
    alloc->bitmap = chunk_start;

    // Mark the whole bitmap as free
    memset(alloc->bitmap, 0, bitmap_size);

    // Mark the space occupied by the bitmap itself as used
    bitmap_set_region(alloc->bitmap, 0, DIV_ROUND_UP(bitmap_size, block_size));

    return true;
}

static usize _first_fit(bitmap_alloc* alloc, usize bytes) {
    usize region_bottom = 0, region_size = 0;

    for (usize word = 0; word < alloc->word_count; word++) {
        if (alloc->bitmap[word] == ~(bitmap_word)0) {
            region_size = 0;
            region_bottom = (word + 1) * BITMAP_WORD_SIZE;
            continue;
        }

        for (usize bit = 0; bit < BITMAP_WORD_SIZE; bit++) {
            if ((alloc->bitmap[word] >> bit) & 1) {
                region_size = 0;
                region_bottom = word * BITMAP_WORD_SIZE + bit + 1;
                continue;
            }

            region_size++;

            if (region_size >= bytes)
                return region_bottom;
        }
    }

    return ALLOC_OUT_OF_BLOCKS;
}


void* bitmap_alloc_malloc(bitmap_alloc* alloc, usize bytes) {
    if (bytes == 0)
        return NULL;

    usize block_count = DIV_ROUND_UP(bytes, alloc->block_size);

    // TODO: implement more allocation strategies
    usize first_block = _first_fit(alloc, bytes);

    if (first_block == ALLOC_OUT_OF_BLOCKS)
        return NULL;

    bitmap_set_region(alloc->bitmap, first_block, block_count);

    return _to_ptr(alloc->block_size, (uptr)alloc->chuck_start, first_block);
}
