#include "bitmap.h"

#include <base/macros.h>
#include <base/types.h>
#include <data/bitmap.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

bool bitmap_alloc_init(bitmap_allocator_t *alloc, void *chunk_start, size_t chunk_size, size_t block_size) {
    if (!alloc || !chunk_start || !chunk_size || !block_size) {
        return false;
    }

    size_t block_count = chunk_size / block_size;
    if (!block_count) {
        return false;
    }

    size_t word_count = DIV_ROUND_UP(block_count, BITMAP_WORD_SIZE);
    size_t bitmap_bytes = DIV_ROUND_UP(block_count, CHAR_BIT);
    size_t bitmap_blocks = DIV_ROUND_UP(bitmap_bytes, block_size);

    if (!bitmap_blocks || bitmap_blocks >= block_count) {
        return false;
    }

    alloc->chuck_start = chunk_start;
    alloc->chunk_size = chunk_size;
    alloc->block_size = block_size;
    alloc->block_count = block_count;
    alloc->word_count = word_count;

    // Place the bitmap at the start of the chunk
    alloc->bitmap = chunk_start;

    // Mark the whole bitmap as free
    memset(alloc->bitmap, 0, bitmap_bytes);
    alloc->free_blocks = alloc->block_count - bitmap_blocks;
    alloc->usable_blocks = alloc->free_blocks;

    // Mark the space occupied by the bitmap itself as used
    bitmap_set_region(alloc->bitmap, 0, bitmap_blocks);
    alloc->next_fit_block = bitmap_blocks;

    return true;
}

static bool first_fit_in_range(bitmap_allocator_t *alloc, size_t blocks, size_t begin, size_t end, size_t *out_block) {
    if (!alloc || !blocks || !out_block || begin >= end) {
        return false;
    }

    size_t region_bottom = 0;
    size_t region_size = 0;

    for (size_t block = begin; block < end; block++) {
        size_t bit = block % BITMAP_WORD_SIZE;
        size_t word = block / BITMAP_WORD_SIZE;
        bitmap_word_t bits = alloc->bitmap[word];

        if (bit == 0 && bits == (bitmap_word_t)-1 && (end - block) >= BITMAP_WORD_SIZE) {
            region_size = 0;
            block += BITMAP_WORD_SIZE - 1;
            continue;
        }

        if (bits & ((bitmap_word_t)1U << bit)) {
            region_size = 0;
            continue;
        }

        if (!region_size) {
            region_bottom = block;
        }

        region_size++;

        if (region_size == blocks) {
            *out_block = region_bottom;
            return true;
        }
    }

    return false;
}

static bool _first_fit(bitmap_allocator_t *alloc, size_t blocks, size_t *out_block) {
    if (!alloc || !blocks || !out_block) {
        return false;
    }

    size_t start = alloc->next_fit_block;
    if (start >= alloc->block_count) {
        start = 0;
    }

    if (first_fit_in_range(alloc, blocks, start, alloc->block_count, out_block)) {
        return true;
    }

    return start && first_fit_in_range(alloc, blocks, 0, start, out_block);
}

static bool _last_fit(bitmap_allocator_t *alloc, size_t blocks, size_t *out_block) {
    if (!alloc || !blocks || !out_block) {
        return false;
    }

    size_t region_size = 0;
    size_t region_top = alloc->block_count;

    for (size_t word = alloc->word_count; word > 0; word--) {
        size_t word_index = word - 1;

        if (alloc->bitmap[word_index] == (bitmap_word_t)-1) {
            region_size = 0;
            region_top = word_index * BITMAP_WORD_SIZE;
            continue;
        }

        for (size_t bit = BITMAP_WORD_SIZE; bit > 0; bit--) {
            size_t bit_index = bit - 1;
            size_t block = word_index * BITMAP_WORD_SIZE + bit_index;

            if (block >= alloc->block_count) {
                region_size = 0;
                region_top = block;
                continue;
            }

            if (alloc->bitmap[word_index] & ((bitmap_word_t)1U << bit_index)) {
                region_size = 0;
                region_top = block;
                continue;
            }

            region_size++;

            if (region_size == blocks) {
                *out_block = region_top - blocks;
                return true;
            }
        }
    }

    return false;
}

void *bitmap_alloc_reserve(bitmap_allocator_t *alloc, size_t blocks) {
    if (!alloc || !blocks || blocks > alloc->free_blocks) {
        return NULL;
    }

    size_t first_block = 0;
    if (!_first_fit(alloc, blocks, &first_block)) {
        return NULL;
    }

    bitmap_set_region(alloc->bitmap, first_block, blocks);
    alloc->next_fit_block = first_block + blocks;

    if (alloc->next_fit_block >= alloc->block_count) {
        alloc->next_fit_block = 0;
    }

    alloc->free_blocks -= blocks;

    return bitmap_alloc_to_ptr(alloc, first_block);
}

void *bitmap_alloc_reserve_high(bitmap_allocator_t *alloc, size_t blocks) {
    if (!alloc || !blocks || blocks > alloc->free_blocks) {
        return NULL;
    }

    size_t first_block = 0;
    if (!_last_fit(alloc, blocks, &first_block)) {
        return NULL;
    }

    bitmap_set_region(alloc->bitmap, first_block, blocks);

    alloc->free_blocks -= blocks;

    return bitmap_alloc_to_ptr(alloc, first_block);
}

bool bitmap_alloc_free(bitmap_allocator_t *alloc, void *ptr, size_t blocks) {
    bool bad_alloc = !alloc || !ptr || !blocks || !alloc->block_size || !alloc->block_count || !alloc->chunk_size;

    if (bad_alloc) {
        return false;
    }

    uintptr_t start = (uintptr_t)alloc->chuck_start;
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t end = start + alloc->chunk_size;

    bool outside_chunk = end <= start || addr < start || addr >= end;
    bool bad_layout = alloc->chunk_size / alloc->block_size < alloc->block_count;

    if (outside_chunk || bad_layout) {
        return false;
    }

    if ((addr - start) % alloc->block_size) {
        return false;
    }

    size_t first_block = bitmap_alloc_to_block(alloc, ptr);
    if (first_block >= alloc->block_count || blocks > alloc->block_count - first_block) {
        return false;
    }

    // Catch double-free before we clear anything. The heap trusts this check.
    for (size_t i = 0; i < blocks; i++) {
        if (!bitmap_get(alloc->bitmap, first_block + i)) {
            return false;
        }
    }

    bitmap_clear_region(alloc->bitmap, first_block, blocks);

    if (first_block < alloc->next_fit_block) {
        alloc->next_fit_block = first_block;
    }

    if (blocks > alloc->block_count - alloc->free_blocks) {
        alloc->free_blocks = alloc->block_count;
    } else {
        alloc->free_blocks += blocks;
    }

    return true;
}
