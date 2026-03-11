#include "vector.h"

#include <base/types.h>
#include <stdlib.h>
#include <string.h>


static size_t _vec_grow_capacity(size_t current, size_t needed) {
    if (current >= needed) {
        return current;
    }

    size_t capacity = current ? current : VEC_INITIAL_CAPACITY;
    if (capacity < needed && capacity == 0) {
        capacity = needed;
    }

    while (capacity < needed) {
        size_t grown = capacity * VEC_GROWTH_RATE;
        if (grown <= capacity) {
            return needed;
        }

        capacity = grown;
    }

    return capacity;
}


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

size_t vec_size(const vector_t *vec) {
    return vec ? vec->size : 0;
}

size_t vec_capacity(const vector_t *vec) {
    return vec ? vec->capacity : 0;
}

bool vec_resize(vector_t *vec, size_t size) {
    if (!vec) {
        return false;
    }

    if (size > vec->capacity && !vec_reserve(vec, size)) {
        return false;
    }

    u8 *base = vec->data;
    if (size > vec->size) {
        memset(
            base + (vec->size * vec->elem_size),
            0,
            (size - vec->size) * vec->elem_size
        );
    } else if (size < vec->size) {
        memset(
            base + (size * vec->elem_size),
            0,
            (vec->size - size) * vec->elem_size
        );
    }

    vec->size = size;
    return true;
}


bool vec_reserve(vector_t *vec, size_t capacity) {
    if (!vec) {
        return false;
    }

    if (capacity <= vec->capacity) {
        return true;
    }

    size_t new_capacity = _vec_grow_capacity(vec->capacity, capacity);
    if (!new_capacity) {
        return false;
    }

    u8 *old_buf = vec->data;
    u8 *new_buf = calloc(new_capacity, vec->elem_size);

    if (!new_buf) {
        return false;
    }

    memcpy(new_buf, old_buf, vec->size * vec->elem_size);
    free(old_buf);

    vec->data = new_buf;
    vec->capacity = new_capacity;

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
        if (!vec_reserve(vec, vec->size + 1)) {
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
