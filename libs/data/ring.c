#include "ring.h"

#include <base/types.h>
#include <stdlib.h>
#include <string.h>

void ring_io_init(ring_io_t *ring, u8 *data, size_t capacity) {
    if (!ring) {
        return;
    }

    ring->data = data;
    ring->capacity = capacity;
    ring_io_reset(ring);
}

void ring_io_reset(ring_io_t *ring) {
    if (!ring) {
        return;
    }

    ring->read_pos = 0;
    ring->write_pos = 0;
    ring->size = 0;
}

size_t ring_io_size(const ring_io_t *ring) {
    return ring ? ring->size : 0;
}

size_t ring_io_free_space(const ring_io_t *ring) {
    if (!ring || ring->capacity < ring->size) {
        return 0;
    }

    return ring->capacity - ring->size;
}

size_t ring_io_read(ring_io_t *ring, void *out, size_t len) {
    if (!ring || !ring->data || !out || !len || !ring->size || !ring->capacity) {
        return 0;
    }

    size_t chunk = len;
    if (chunk > ring->size) {
        chunk = ring->size;
    }

    size_t first = chunk;
    if (first > ring->capacity - ring->read_pos) {
        first = ring->capacity - ring->read_pos;
    }

    memcpy(out, ring->data + ring->read_pos, first);
    if (chunk > first) {
        memcpy((u8 *)out + first, ring->data, chunk - first);
    }

    ring->read_pos = (ring->read_pos + chunk) % ring->capacity;
    ring->size -= chunk;

    return chunk;
}

size_t ring_io_write(ring_io_t *ring, const void *in, size_t len) {
    if (!ring || !ring->data || !in || !len || !ring->capacity) {
        return 0;
    }

    size_t free_space = ring_io_free_space(ring);
    if (!free_space) {
        return 0;
    }

    size_t chunk = len;
    if (chunk > free_space) {
        chunk = free_space;
    }

    size_t first = chunk;
    if (first > ring->capacity - ring->write_pos) {
        first = ring->capacity - ring->write_pos;
    }

    memcpy(ring->data + ring->write_pos, in, first);
    if (chunk > first) {
        memcpy(ring->data, (const u8 *)in + first, chunk - first);
    }

    ring->write_pos = (ring->write_pos + chunk) % ring->capacity;
    ring->size += chunk;

    return chunk;
}

static size_t ring_mask(const ring_buffer_t *ring) {
    return ring->size - 1;
}

static size_t ring_capacity(const ring_buffer_t *ring) {
    return ring_mask(ring);
}

static size_t ring_used(const ring_buffer_t *ring) {
    size_t mask = ring_mask(ring);
    return (ring->head_index - ring->tail_index) & mask;
}

static size_t ring_free(const ring_buffer_t *ring) {
    return ring_capacity(ring) - ring_used(ring);
}


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
    return ring_used(ring) == ring_capacity(ring);
}

bool ring_buffer_is_empty(ring_buffer_t *ring) {
    return ring->head_index == ring->tail_index;
}


void ring_buffer_clear(ring_buffer_t *ring) {
    ring->head_index = ring->tail_index;
}

void ring_buffer_push(ring_buffer_t *ring, u8 data) {
    if (ring_buffer_is_full(ring)) {
        ring->tail_index = (ring->tail_index + 1) & ring_mask(ring);
    }

    ring->buffer[ring->head_index] = data;
    ring->head_index = (ring->head_index + 1) & ring_mask(ring);
}

void ring_buffer_push_array(ring_buffer_t *ring, u8 *data, size_t len) {
    if (!ring || !data || !len || ring->size < 2) {
        return;
    }

    size_t capacity = ring_capacity(ring);
    size_t mask = ring_mask(ring);

    if (len > capacity) {
        data += len - capacity;
        len = capacity;
        ring->tail_index = ring->head_index;
    } else {
        size_t free_bytes = ring_free(ring);
        if (len > free_bytes) {
            size_t drop = len - free_bytes;
            ring->tail_index = (ring->tail_index + drop) & mask;
        }
    }

    for (size_t i = 0; i < len; i++) {
        ring->buffer[ring->head_index] = data[i];
        ring->head_index = (ring->head_index + 1) & mask;
    }
}


bool ring_buffer_pop(ring_buffer_t *ring, u8 *ret) {
    if (ring_buffer_is_empty(ring)) {
        return false;
    }

    if (ret) {
        *ret = ring->buffer[ring->tail_index];
    }

    ring->tail_index = (ring->tail_index + 1) & ring_mask(ring);

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
