#pragma once

#include <base/types.h>

void pmm_init(u64 mem_base, u64 mem_size, u64 reserved_end);

size_t pmm_total_mem(void);
size_t pmm_free_mem(void);

void *alloc_frames(size_t count);
void *alloc_frames_high(size_t count);
void *alloc_frames_user(size_t count);
void free_frames(void *ptr, size_t count);

void pmm_ref_init(void);
bool pmm_ref_ready(void);
void pmm_ref_hold(void *ptr, size_t blocks);
u16 pmm_refcount(void *ptr);
