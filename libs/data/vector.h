#pragma once

#include <base/types.h>

// A C++ style dynamic array. Not the ideal name for such a structure but it stuck I guess

#define VEC_GROWTH_RATE      2
#define VEC_INITIAL_CAPACITY 4

typedef struct vector {
    size_t size;
    size_t capacity;

    size_t elem_size;
    void *data;
} vector_t;


vector_t *vec_create(size_t elem_size);
vector_t *vec_create_sized(size_t capacity, size_t elem_size);

void vec_destroy(vector_t *vec);

vector_t *vec_clone(vector_t *parent);

bool vec_reserve(vector_t *vec, size_t capacity);
bool vec_reserve_more(vector_t *vec, size_t additional);

void *vec_at(vector_t *vec, size_t index);
bool vec_get(vector_t *vec, size_t index, void *ret);
void *vec_set(vector_t *vec, size_t index, void *data);

bool vec_clear(vector_t *vec);

bool vec_insert(vector_t *vec, size_t index, void *data);

bool vec_swap(vector_t *vec, size_t i, size_t j);

bool vec_push(vector_t *vec, void *data);
bool vec_push_array(vector_t *vec, void *array, size_t len);

bool vec_pop(vector_t *vec, void *ret);
size_t vec_pop_array(vector_t *vec, void *ret, size_t len);
