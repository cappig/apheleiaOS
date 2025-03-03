#pragma once

#include <base/types.h>

typedef struct {
    u8* buffer;
    usize size;

    usize head_index;
    usize tail_index;
} ring_buffer;


ring_buffer* ring_buffer_create(usize size);
void ring_buffer_destroy(ring_buffer* ring);

bool ring_buffer_is_full(ring_buffer* ring);
bool ring_buffer_is_empty(ring_buffer* ring);

void ring_buffer_clear(ring_buffer* ring);

void ring_buffer_push(ring_buffer* ring, u8 data);
void ring_buffer_push_array(ring_buffer* ring, u8* data, usize len);

bool ring_buffer_pop(ring_buffer* ring, u8* ret);
usize ring_buffer_pop_array(ring_buffer* ring, u8* ret, usize len);
