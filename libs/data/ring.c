#include "ring.h"

#include <base/types.h>
#include <stdlib.h>


ring_buffer_t *ring_buffer_create(size_t size) {
    ring_buffer_t *ret = calloc(1, sizeof(ring_buffer_t));

    if (!ret) {
        return NULL;
    }

    ret->size = size;
    ret->buffer = malloc(size);

    if (!ret->buffer) {
        free(ret);
        return NULL;
    }

    return ret;
}

void ring_buffer_destroy(ring_buffer_t *ring) {
    if (!ring) {
        return;
    }

    if (ring->buffer) {
        free(ring->buffer);
    }

    free(ring);
}


bool ring_buffer_is_full(ring_buffer_t *ring) {
    size_t mask = ring->size - 1;
    size_t diff = ring->head_index - ring->tail_index;

    return (diff & mask) == mask;
}

bool ring_buffer_is_empty(ring_buffer_t *ring) {
    return ring->head_index == ring->tail_index;
}


void ring_buffer_clear(ring_buffer_t *ring) {
    ring->head_index = ring->tail_index;
}

void ring_buffer_push(ring_buffer_t *ring, u8 data) {
    if (ring_buffer_is_full(ring)) {
        ring->tail_index += 1;
        ring->tail_index &= ring->size - 1;
    }

    ring->buffer[ring->head_index] = data;
    ring->head_index += 1;
    ring->head_index &= ring->size - 1;
}

void ring_buffer_push_array(ring_buffer_t *ring, u8 *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ring_buffer_push(ring, data[i]);
    }
}


bool ring_buffer_pop(ring_buffer_t *ring, u8 *ret) {
    if (ring_buffer_is_empty(ring)) {
        return false;
    }

    if (ret) {
        *ret = ring->buffer[ring->tail_index];
    }

    ring->tail_index += 1;
    ring->tail_index &= ring->size - 1;

    return true;
}

size_t ring_buffer_pop_array(ring_buffer_t *ring, u8 *ret, size_t len) {
    if (ring_buffer_is_empty(ring)) {
        return 0;
    }

    u8 *pos = ret;
    size_t i = 0;

    while ((i < len) && ring_buffer_pop(ring, pos)) {
        i++;

        if (pos) {
            pos++;
        }
    }

    return i;
}
