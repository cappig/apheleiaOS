#include "vector.h"

#include <base/types.h>
#include <stdlib.h>
#include <string.h>


vector_t *vec_create_sized(size_t capacity, size_t elem_size) {
    vector_t *vec = calloc(1, sizeof(vector_t));

    if (!vec) {
        return NULL;
    }

    vec->size = 0;

    vec->elem_size = elem_size;
    vec->capacity = capacity;

    vec->data = calloc(vec->capacity, elem_size);

    if (!vec->data) {
        free(vec);
        return NULL;
    }

    return vec;
}

vector_t *vec_create(size_t elem_size) {
    return vec_create_sized(VEC_INITIAL_CAPACITY, elem_size);
}

void vec_destroy(vector_t *vec) {
    if (!vec) {
        return;
    }

    if (vec->data) {
        free(vec->data);
    }

    free(vec);
}


vector_t *vec_clone(vector_t *parent) {
    if (!parent) {
        return NULL;
    }

    vector_t *child = vec_create_sized(parent->capacity, parent->elem_size);

    if (!child) {
        return NULL;
    }

    size_t size = parent->elem_size * parent->capacity;
    memcpy(child->data, parent->data, size);

    child->size = parent->size;

    return child;
}


bool vec_reserve(vector_t *vec, size_t capacity) {
    u8 *old_buf = vec->data;
    u8 *new_buf = calloc(capacity, vec->elem_size);

    if (!new_buf) {
        return false;
    }

    memcpy(new_buf, old_buf, vec->size * vec->elem_size);
    free(old_buf);

    vec->data = new_buf;
    vec->capacity = capacity;

    return true;
}

bool vec_reserve_more(vector_t *vec, size_t additional) {
    return vec_reserve(vec, vec->capacity + additional);
}


void *vec_at(vector_t *vec, size_t index) {
    if (!vec || index >= vec->capacity) {
        return NULL;
    }

    return vec->data + index * vec->elem_size;
}

void *vec_at_ptr(vector_t *vec, size_t index) {
    if (!vec || vec->elem_size != sizeof(void *)) {
        return NULL;
    }

    void **slot = vec_at(vec, index);
    if (!slot) {
        return NULL;
    }

    return *slot;
}

bool vec_get(vector_t *vec, size_t index, void *ret) {
    void *ptr = vec_at(vec, index);

    if (!ptr) {
        return false;
    }

    memcpy(ret, ptr, vec->elem_size);

    return true;
}

void *vec_set(vector_t *vec, size_t index, void *data) {
    if (!vec || !data || index >= vec->capacity) {
        return NULL;
    }

    void *ptr = vec->data + index * vec->elem_size;
    memcpy(ptr, data, vec->elem_size);

    return ptr;
}


bool vec_clear(vector_t *vec) {
    void *ptr = vec->data;
    size_t len = vec->size * vec->elem_size;

    memset(ptr, 0, len);

    vec->size = 0;

    return true;
}


bool vec_insert(vector_t *vec, size_t index, void *data) {
    if (!vec || !data) {
        return false;
    }

    if (index >= vec->capacity) {
        if (!vec_reserve(vec, index + 1)) {
            return false;
        }
    }

    vec->size = max(vec->size, index + 1);

    return vec_set(vec, index, data);
}


bool vec_swap(vector_t *vec, size_t i, size_t j) {
    void *first = vec_at(vec, i);
    if (!first) {
        return false;
    }

    void *second = vec_at(vec, j);
    if (!second) {
        return false;
    }

    memswap(first, second, vec->elem_size);

    return true;
}


bool vec_push(vector_t *vec, void *data) {
    if (vec->size == vec->capacity) {
        if (!vec_reserve(vec, vec->capacity * VEC_GROWTH_RATE)) {
            return false;
        }
    }

    vec_set(vec, vec->size, data);

    vec->size++;

    return true;
}

bool vec_push_array(vector_t *vec, void *array, size_t len) {
    if (!vec || (!array && len)) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        void *data = array + i * vec->elem_size;
        if (!vec_push(vec, data)) {
            return false;
        }
    }

    return true;
}


bool vec_pop(vector_t *vec, void *ret) {
    if (!vec->size) {
        return false;
    }

    vec->size--;

    if (ret) {
        vec_get(vec, vec->size, ret);
    }

    return true;
}

size_t vec_pop_array(vector_t *vec, void *ret, size_t len) {
    if (!vec || !vec->size || !len) {
        return 0;
    }

    u8 *pos = ret;
    size_t i = 0;

    while ((i < len) && vec_pop(vec, pos)) {
        i++;

        if (pos) {
            pos += vec->elem_size;
        }
    }

    return i;
}

bool vec_remove_at(vector_t *vec, size_t index, void *ret) {
    if (!vec || index >= vec->size) {
        return false;
    }

    u8 *base = (u8 *)vec->data;
    u8 *slot = base + (index * vec->elem_size);

    if (ret) {
        memcpy(ret, slot, vec->elem_size);
    }

    if (index + 1 < vec->size) {
        size_t tail_count = vec->size - index - 1;
        memmove(slot, slot + vec->elem_size, tail_count * vec->elem_size);
    }

    vec->size--;
    memset(base + (vec->size * vec->elem_size), 0, vec->elem_size);

    return true;
}
