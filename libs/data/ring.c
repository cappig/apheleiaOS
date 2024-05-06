#include "ring.h"

#include <base/types.h>

#include "mem/heap.h"


ring_buffer* ring_buffer_create(usize size) {
    ring_buffer* ret = kcalloc(sizeof(ring_buffer));

    ret->size = size;
    ret->buffer = kcalloc(size);

    return ret;
}

void ring_buffer_destroy(ring_buffer* ring) {
    kfree(ring->buffer);
    kfree(ring);
}


bool ring_buffer_is_full(ring_buffer* ring) {
    usize mask = ring->size - 1;
    usize diff = ring->head_index - ring->tail_index;

    return (diff & mask) == mask;
}

bool ring_buffer_is_empty(ring_buffer* ring) {
    return ring->head_index == ring->tail_index;
}


void ring_buffer_push(ring_buffer* ring, u8 data) {
    if (ring_buffer_is_full(ring)) {
        ring->tail_index += 1;
        ring->tail_index &= ring->size - 1;
    }

    ring->buffer[ring->head_index] = data;
    ring->head_index += 1;
    ring->head_index &= ring->size - 1;
}

void ring_buffer_push_array(ring_buffer* ring, u8* data, usize len) {
    for (usize i = 0; i < len; i++)
        ring_buffer_push(ring, data[i]);
}

bool ring_buffer_pop(ring_buffer* ring, u8* ret) {
    if (ring_buffer_is_empty(ring))
        return 0;

    *ret = ring->buffer[ring->tail_index];
    ring->tail_index += 1;
    ring->tail_index &= ring->size - 1;

    return 1;
}

usize ring_buffer_pop_array(ring_buffer* ring, u8* ret, usize len) {
    if (ring_buffer_is_empty(ring))
        return 0;

    u8* pos = ret;
    usize i = 0;
    while ((i < len) && ring_buffer_pop(ring, pos)) {
        i++;
        pos++;
    }

    return i;
}
