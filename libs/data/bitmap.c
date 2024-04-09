#include "bitmap.h"

#include <base/types.h>


void bitmap_set(bitmap_word* bitmap, usize index) {
    u64 word = index / BITMAP_WORD_SIZE;
    u64 bit = index % BITMAP_WORD_SIZE;

    bitmap[word] |= (1 << bit);
}

void bitmap_clear(bitmap_word* bitmap, usize index) {
    u64 word = index / BITMAP_WORD_SIZE;
    u64 bit = index % BITMAP_WORD_SIZE;

    bitmap[word] &= ~(1 << bit);
}

void bitmap_set_region(bitmap_word* bitmap, usize index, usize blocks) {
    for (usize i = 0; i < blocks; i++)
        bitmap_set(bitmap, index + i);
}

void bitmap_clear_region(bitmap_word* bitmap, usize index, usize blocks) {
    for (usize i = 0; i < blocks; i++)
        bitmap_clear(bitmap, index + i);
}

bool bitmap_get(bitmap_word* bitmap, usize index) {
    u64 word = index / BITMAP_WORD_SIZE;
    u64 bit = index % BITMAP_WORD_SIZE;

    return ((bitmap[word] >> bit) & 1);
}
