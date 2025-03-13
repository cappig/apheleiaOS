#pragma once

#include <base/types.h>
#include <limits.h>
#include <stdbool.h>

#define BITMAP_WORD_SIZE (sizeof(bitmap_word) * CHAR_BIT)

typedef u32 bitmap_word;


void bitmap_set(bitmap_word* bitmap, usize index);
void bitmap_clear(bitmap_word* bitmap, usize index);

void bitmap_set_region(bitmap_word* bitmap, usize index, usize blocks);
void bitmap_clear_region(bitmap_word* bitmap, usize index, usize blocks);

bool bitmap_get(bitmap_word* bitmap, usize index);
