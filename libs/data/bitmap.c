#include "bitmap.h"

#include <base/types.h>

void bitmap_set(bitmap_word_t *bitmap, size_t index) {
    u64 word = index / BITMAP_WORD_SIZE;
    u64 bit = index % BITMAP_WORD_SIZE;

    bitmap[word] |= ((bitmap_word_t)1U << bit);
}

void bitmap_clear(bitmap_word_t *bitmap, size_t index) {
    u64 word = index / BITMAP_WORD_SIZE;
    u64 bit = index % BITMAP_WORD_SIZE;

    bitmap[word] &= ~((bitmap_word_t)1U << bit);
}

void bitmap_set_region(bitmap_word_t *bitmap, size_t index, size_t blocks) {
    for (size_t i = 0; i < blocks; i++) {
        bitmap_set(bitmap, index + i);
    }
}

void bitmap_clear_region(bitmap_word_t *bitmap, size_t index, size_t blocks) {
    for (size_t i = 0; i < blocks; i++) {
        bitmap_clear(bitmap, index + i);
    }
}

bool bitmap_get(bitmap_word_t *bitmap, size_t index) {
    u64 word = index / BITMAP_WORD_SIZE;
    u64 bit = index % BITMAP_WORD_SIZE;

    return ((bitmap[word] >> bit) & 1U) != 0;
}

bool bitmap_find_first_clear(
    const bitmap_word_t *bitmap,
    size_t bit_count,
    size_t *index_out
) {
    if (!bitmap || !index_out) {
        return false;
    }

    size_t words = bit_count / BITMAP_WORD_SIZE;
    size_t rem = bit_count % BITMAP_WORD_SIZE;

    for (size_t w = 0; w < words; w++) {
        bitmap_word_t bits = bitmap[w];
        if (bits == (bitmap_word_t)-1) {
            continue;
        }

        for (size_t bit = 0; bit < BITMAP_WORD_SIZE; bit++) {
            if (!(bits & ((bitmap_word_t)1U << bit))) {
                *index_out = w * BITMAP_WORD_SIZE + bit;
                return true;
            }
        }
    }

    if (!rem) {
        return false;
    }

    bitmap_word_t tail = bitmap[words];
    for (size_t bit = 0; bit < rem; bit++) {
        if (!(tail & ((bitmap_word_t)1U << bit))) {
            *index_out = words * BITMAP_WORD_SIZE + bit;
            return true;
        }
    }

    return false;
}
