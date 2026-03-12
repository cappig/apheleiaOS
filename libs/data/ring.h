#pragma once

#include <base/types.h>

typedef struct ring {
    u8 *buffer;
    size_t size;

    size_t head_index;
    size_t tail_index;
} ring_buffer_t;

typedef struct ring_io {
    u8 *data;
    size_t capacity;
    size_t read_pos;
    size_t write_pos;
    size_t size;
} ring_io_t;


void ring_io_init(ring_io_t *ring, u8 *data, size_t capacity);
void ring_io_reset(ring_io_t *ring);

size_t ring_io_size(const ring_io_t *ring);
size_t ring_io_free_space(const ring_io_t *ring);

size_t ring_io_read(ring_io_t *ring, void *out, size_t len);
size_t ring_io_write(ring_io_t *ring, const void *in, size_t len);

ring_buffer_t *ring_buffer_create(size_t size);
void ring_buffer_destroy(ring_buffer_t *ring);

bool ring_buffer_is_full(ring_buffer_t *ring);
bool ring_buffer_is_empty(ring_buffer_t *ring);

void ring_buffer_clear(ring_buffer_t *ring);

void ring_buffer_push(ring_buffer_t *ring, u8 data);
void ring_buffer_push_array(ring_buffer_t *ring, u8 *data, size_t len);

bool ring_buffer_pop(ring_buffer_t *ring, u8 *ret);
size_t ring_buffer_pop_array(ring_buffer_t *ring, u8 *ret, size_t len);

// generic typed ring queue backed by a heap-allocated circular buffer
typedef struct ring_queue {
    u8    *buf;
    size_t elem_size;
    size_t cap;
    size_t head;
    size_t count;
} ring_queue_t;

ring_queue_t *ring_queue_create(size_t elem_size, size_t cap);
void ring_queue_destroy(ring_queue_t *q);

size_t ring_queue_count(const ring_queue_t *q);
size_t ring_queue_capacity(const ring_queue_t *q);

bool ring_queue_push(ring_queue_t *q, const void *item);
bool ring_queue_pop(ring_queue_t *q, void *out);

void ring_queue_drop_head(ring_queue_t *q);

void *ring_queue_at(ring_queue_t *q, size_t i);
bool ring_queue_remove_at(ring_queue_t *q, size_t i);

void ring_queue_clear(ring_queue_t *q);
bool ring_queue_reserve(ring_queue_t *q, size_t needed);
