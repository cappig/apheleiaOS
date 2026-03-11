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
