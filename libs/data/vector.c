#include "vector.h"

#include <alloc/global.h>
#include <string.h>

#include "base/types.h"


vector* vec_create_sized(usize size) {
    vector* vec = gcalloc(sizeof(vector));

    if (!vec)
        return NULL;

    vec->data = gmalloc(size);

    if (!vec->data) {
        gfree(vec);
        return NULL;
    }

    vec->capacity = size;
    vec->size = 0;

    return vec;
}

vector* vec_create() {
    return vec_create_sized(VEC_INITIAL_SIZE);
}

void vec_destroy(vector* vec) {
    if (!vec)
        return;

    if (vec->data)
        gfree(vec->data);

    gfree(vec);
}


bool vec_push(vector* vec, u8 data) {
    if (vec->size == vec->capacity) {
        usize new_size = vec->size * VEC_GROWTH_RATE;

        u8* old_buf = vec->data;
        u8* new_buf = gmalloc(new_size);

        if (!new_buf)
            return false;

        memcpy(new_buf, old_buf, vec->size);
        gfree(old_buf);

        vec->data = new_buf;
    }

    vec->data[vec->size] = data;

    vec->size++;

    return true;
}

bool vec_push_array(vector* vec, u8* data, usize len) {
    for (usize i = 0; i < len; i++)
        vec_push(vec, data[i]);

    return true;
}


bool vec_pop(vector* vec, u8* ret) {
    if (!vec->size)
        return false;

    vec->size--;

    if (ret)
        *ret = vec->data[vec->size];

    return true;
}

usize vec_pop_array(vector* vec, u8* ret, usize len) {
    if (!vec->size)
        return false;

    u8* pos = ret;
    usize i = 0;

    while ((i < len) && vec_pop(vec, pos)) {
        i++;

        if (pos)
            pos++;
    }

    return true;
}
