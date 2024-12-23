#pragma once

#include <base/types.h>

// A C++ style dynamic array. Not the ideal name for such a structure but it stuck I guess

#define VEC_GROWTH_RATE  2
#define VEC_INITIAL_SIZE 4

typedef struct {
    usize size;
    usize capacity;

    u8* data;
} vector;


vector* vec_create(void);
vector* vec_create_sized(usize size);

void vec_destroy(vector* vec);

bool vec_push(vector* vec, u8 data);
bool vec_push_array(vector* vec, u8* data, usize len);

bool vec_pop(vector* vec, u8* ret);
usize vec_pop_array(vector* vec, u8* ret, usize len);
