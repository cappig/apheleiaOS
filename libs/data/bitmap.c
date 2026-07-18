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

static bitmap_word_t low_mask(size_t bits) {
    if (bits >= BITMAP_WORD_SIZE) {
        return (bitmap_word_t)-1;
    }

    return ((bitmap_word_t)1U << bits) - 1U;
}

static void write_region(bitmap_word_t *bitmap, size_t index, size_t blocks, bool set) {
    if (!blocks) {
        return;
    }

    size_t word = index / BITMAP_WORD_SIZE;
    size_t bit = index % BITMAP_WORD_SIZE;

    if (bit) {
        size_t count = BITMAP_WORD_SIZE - bit;
        if (count > blocks) {
            count = blocks;
        }

        bitmap_word_t mask = low_mask(count) << bit;
        if (set) {
            bitmap[word] |= mask;
        } else {
            bitmap[word] &= ~mask;
        }

        blocks -= count;
        word++;
    }

    bitmap_word_t fill = set ? (bitmap_word_t)-1 : 0;
    while (blocks >= BITMAP_WORD_SIZE) {
        bitmap[word++] = fill;
        blocks -= BITMAP_WORD_SIZE;
    }

    if (blocks) {
        bitmap_word_t mask = low_mask(blocks);
        if (set) {
            bitmap[word] |= mask;
        } else {
            bitmap[word] &= ~mask;
        }
    }
}

void bitmap_set_region(bitmap_word_t *bitmap, size_t index, size_t blocks) {
    write_region(bitmap, index, blocks, true);
}

void bitmap_clear_region(bitmap_word_t *bitmap, size_t index, size_t blocks) {
    write_region(bitmap, index, blocks, false);
}

bool bitmap_get(bitmap_word_t *bitmap, size_t index) {
    u64 word = index / BITMAP_WORD_SIZE;
    u64 bit = index % BITMAP_WORD_SIZE;

    return ((bitmap[word] >> bit) & 1U) != 0;
}

bool bitmap_find_first_clear(const bitmap_word_t *bitmap, size_t bit_count, size_t *index_out) {
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
