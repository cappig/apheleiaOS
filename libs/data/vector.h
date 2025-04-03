#pragma once

#include <base/types.h>

// A C++ style dynamic array. Not the ideal name for such a structure but it stuck I guess

#define VEC_GROWTH_RATE      2
#define VEC_INITIAL_CAPACITY 4

typedef struct {
    usize size;
    usize capacity;

    usize elem_size;
    void* data;
} vector;


vector* vec_create(usize elem_size);
vector* vec_create_sized(usize capacity, usize elem_size);

void vec_destroy(vector* vec);

vector* vec_clone(vector* parent);

bool vec_reserve(vector* vec, usize capacity);
bool vec_reserve_more(vector* vec, usize additional);

void* vec_at(vector* vec, usize index);
bool vec_get(vector* vec, usize index, void* ret);
void* vec_set(vector* vec, usize index, void* data);

bool vec_clear(vector* vec);

bool vec_insert(vector* vec, usize index, void* data);

bool vec_swap(vector* vec, usize i, usize j);

bool vec_push(vector* vec, void* data);
bool vec_push_array(vector* vec, void* array, usize len);

bool vec_pop(vector* vec, void* ret);
usize vec_pop_array(vector* vec, void* ret, usize len);
