#pragma once

#include <base/types.h>
#include <limits.h>
#include <stdbool.h>

typedef u32 bitmap_word_t;


#define BITMAP_WORD_SIZE (sizeof(bitmap_word_t) * CHAR_BIT)

void bitmap_set(bitmap_word_t *bitmap, size_t index);
void bitmap_clear(bitmap_word_t *bitmap, size_t index);

void bitmap_set_region(bitmap_word_t *bitmap, size_t index, size_t blocks);
void bitmap_clear_region(bitmap_word_t *bitmap, size_t index, size_t blocks);

bool bitmap_get(bitmap_word_t *bitmap, size_t index);
bool bitmap_find_first_clear(
    const bitmap_word_t *bitmap,
    size_t bit_count,
    size_t *index_out
);
