#pragma once

#include <base/types.h>

typedef struct ring {
    u8* buffer;
    size_t size;

    size_t head_index;
    size_t tail_index;
} ring_buffer_t;


ring_buffer_t* ring_buffer_create(size_t size);
void ring_buffer_destroy(ring_buffer_t* ring);

bool ring_buffer_is_full(ring_buffer_t* ring);
bool ring_buffer_is_empty(ring_buffer_t* ring);

void ring_buffer_clear(ring_buffer_t* ring);

void ring_buffer_push(ring_buffer_t* ring, u8 data);
void ring_buffer_push_array(ring_buffer_t* ring, u8* data, size_t len);

bool ring_buffer_pop(ring_buffer_t* ring, u8* ret);
size_t ring_buffer_pop_array(ring_buffer_t* ring, u8* ret, size_t len);
