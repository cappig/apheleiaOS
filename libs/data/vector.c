#include "vector.h"

#include <alloc/global.h>
#include <base/types.h>
#include <stdlib.h>
#include <string.h>


vector* vec_create_sized(usize capacity, usize elem_size) {
    vector* vec = gcalloc(sizeof(vector));

    if (!vec)
        return NULL;

    vec->size = 0;

    vec->elem_size = elem_size;
    vec->capacity = capacity;

    vec->data = gmalloc(vec->capacity * elem_size);

    if (!vec->data) {
        gfree(vec);
        return NULL;
    }

    return vec;
}

vector* vec_create(usize elem_size) {
    return vec_create_sized(VEC_INITIAL_CAPACITY, elem_size);
}

void vec_destroy(vector* vec) {
    if (!vec)
        return;

    if (vec->data)
        gfree(vec->data);

    gfree(vec);
}


vector* vec_clone(vector* parent) {
    if (!parent)
        return NULL;

    vector* child = vec_create_sized(parent->capacity, parent->elem_size);

    if (!child)
        return NULL;

    usize size = parent->elem_size * parent->capacity;
    memcpy(child->data, parent->data, size);

    child->size = parent->size;

    return child;
}


bool vec_reserve(vector* vec, usize capacity) {
    u8* old_buf = vec->data;
    u8* new_buf = gcalloc(capacity * vec->elem_size);

    if (!new_buf)
        return false;

    memcpy(new_buf, old_buf, vec->size * vec->elem_size);
    gfree(old_buf);

    vec->data = new_buf;
    vec->capacity = capacity;

    return true;
}

bool vec_reserve_more(vector* vec, usize additional) {
    return vec_reserve(vec, vec->capacity + additional);
}


void* vec_at(vector* vec, usize index) {
    if (index > vec->capacity)
        return NULL;

    return vec->data + index * vec->elem_size;
}

bool vec_get(vector* vec, usize index, void* ret) {
    void* ptr = vec_at(vec, index);

    if (!ptr)
        return false;

    memcpy(ret, ptr, vec->elem_size);

    return true;
}

void* vec_set(vector* vec, usize index, void* data) {
    if (index > vec->capacity)
        return NULL;

    void* ptr = vec->data + index * vec->elem_size;
    memcpy(ptr, data, vec->elem_size);

    return ptr;
}


bool vec_clear(vector* vec) {
    void* ptr = vec->data;
    usize len = vec->size * vec->elem_size;

    memset(ptr, 0, len);

    vec->size = 0;

    return true;
}


bool vec_insert(vector* vec, usize index, void* data) {
    if (index > vec->capacity)
        if (!vec_reserve(vec, index + 1))
            return false;

    vec->size = max(vec->size, index + 1);

    return vec_set(vec, index, data);
}


bool vec_swap(vector* vec, usize i, usize j) {
    void* first = vec_at(vec, i);
    if (!first)
        return false;

    void* second = vec_at(vec, j);
    if (!second)
        return false;

    memswap(first, second, vec->elem_size);

    return true;
}


bool vec_push(vector* vec, void* data) {
    if (vec->size == vec->capacity)
        if (!vec_reserve(vec, vec->capacity * VEC_GROWTH_RATE))
            return false;

    vec_set(vec, vec->size, data);

    vec->size++;

    return true;
}

bool vec_push_array(vector* vec, void* array, usize len) {
    for (usize i = 0; i < len; i++) {
        void* data = array + i * vec->elem_size;
        vec_push(vec, data);
    }

    return true;
}


bool vec_pop(vector* vec, void* ret) {
    if (!vec->size)
        return false;

    vec->size--;

    if (ret)
        vec_get(vec, vec->size, ret);

    return true;
}

usize vec_pop_array(vector* vec, void* ret, usize len) {
    if (!vec->size)
        return false;

    u8* pos = ret;
    usize i = 0;

    while ((i < len) && vec_pop(vec, pos)) {
        i++;

        if (pos)
            pos += vec->elem_size;
    }

    return true;
}
