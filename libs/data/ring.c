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


ring_queue_t *ring_queue_create(size_t elem_size, size_t cap) {
    if (!elem_size || !cap) {
        return NULL;
    }

    ring_queue_t *q = calloc(1, sizeof(*q));
    if (!q) {
        return NULL;
    }

    q->buf = calloc(cap, elem_size);
    if (!q->buf) {
        free(q);
        return NULL;
    }

    q->elem_size = elem_size;
    q->cap = cap;

    return q;
}

void ring_queue_destroy(ring_queue_t *q) {
    if (!q) {
        return;
    }

    free(q->buf);
    free(q);
}

size_t ring_queue_count(const ring_queue_t *q) {
    return q ? q->count : 0;
}

size_t ring_queue_capacity(const ring_queue_t *q) {
    return q ? q->cap : 0;
}

bool ring_queue_push(ring_queue_t *q, const void *item) {
    if (!q || !item || !q->buf || q->count >= q->cap) {
        return false;
    }

    size_t tail = (q->head + q->count) % q->cap;
    memcpy(q->buf + tail * q->elem_size, item, q->elem_size);

    q->count++;

    return true;
}

bool ring_queue_pop(ring_queue_t *q, void *out) {
    if (!q || !q->count || !q->buf) {
        return false;
    }

    if (out) {
        memcpy(out, q->buf + q->head * q->elem_size, q->elem_size);
    }

    q->head = (q->head + 1) % q->cap;
    q->count--;

    return true;
}

void ring_queue_drop_head(ring_queue_t *q) {
    if (!q || !q->count) {
        return;
    }
    q->head = (q->head + 1) % q->cap;
    q->count--;
}

void *ring_queue_at(ring_queue_t *q, size_t i) {
    if (!q || i >= q->count || !q->buf) {
        return NULL;
    }

    return q->buf + ((q->head + i) % q->cap) * q->elem_size;
}

bool ring_queue_remove_at(ring_queue_t *q, size_t i) {
    if (!q || i >= q->count || !q->buf) {
        return false;
    }

    for (size_t j = i; j + 1 < q->count; j++) {
        void *dst = q->buf + ((q->head + j) % q->cap) * q->elem_size;
        void *src = q->buf + ((q->head + j + 1) % q->cap) * q->elem_size;

        memcpy(dst, src, q->elem_size);
    }

    q->count--;

    return true;
}

void ring_queue_clear(ring_queue_t *q) {
    if (!q) {
        return;
    }

    q->head = 0;
    q->count = 0;
}

bool ring_queue_reserve(ring_queue_t *q, size_t needed) {
    if (!q) {
        return false;
    }

    if (q->cap >= needed) {
        return true;
    }

    size_t new_cap = q->cap ? q->cap * 2 : needed;
    if (new_cap < needed) {
        new_cap = needed;
    }

    u8 *new_buf = malloc(new_cap * q->elem_size);
    if (!new_buf) {
        return false;
    }

    if (q->count && q->buf && q->cap) {
        for (size_t i = 0; i < q->count; i++) {
            memcpy(
                new_buf + i * q->elem_size,
                q->buf + ((q->head + i) % q->cap) * q->elem_size,
                q->elem_size
            );
        }
    }

    free(q->buf);

    q->buf = new_buf;
    q->cap = new_cap;
    q->head = 0;

    return true;
}
