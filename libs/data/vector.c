#include "vector.h"

#include <alloc/global.h>
#include <base/types.h>
#include <string.h>


vector* vec_create_sized(usize size, usize elem_size) {
    vector* vec = gcalloc(sizeof(vector));

    if (!vec)
        return NULL;

    vec->size = 0;

    vec->elem_size = elem_size;
    vec->capacity = size * elem_size;

    vec->data = gmalloc(vec->capacity);

    if (!vec->data) {
        gfree(vec);
        return NULL;
    }

    return vec;
}

vector* vec_create(usize elem_size) {
    return vec_create_sized(VEC_INITIAL_SIZE, elem_size);
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
    u8* new_buf = gmalloc(capacity);

    if (!new_buf)
        return false;

    memcpy(new_buf, old_buf, vec->size);
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


bool vec_insert(vector* vec, usize index, void* data) {
    usize capacity = vec->capacity;

    if (index > capacity) {
        vec_reserve(vec, index + 1);
        vec->size = index + 1;

        void* ptr = vec->data + index * vec->elem_size;
        memset(ptr, 0, index - capacity);
    }

    return vec_set(vec, index, data);
}


bool vec_push(vector* vec, void* data) {
    if (vec->size == vec->capacity) {
        usize capacity = vec->size * VEC_GROWTH_RATE;

        if (!vec_reserve(vec, capacity))
            return false;
    }

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
