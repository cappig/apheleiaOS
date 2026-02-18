#pragma once

#include <base/types.h>
#include <x86/e820.h>


void pmm_init(e820_map_t *mmap);

size_t pmm_total_mem(void);
size_t pmm_free_mem(void);

void *alloc_frames(size_t count);
void *alloc_frames_high(size_t count);
void *alloc_frames_user(size_t count);
void free_frames(void *ptr, size_t size);

void pmm_ref_init(void);
bool pmm_ref_ready(void);
void pmm_ref_hold(void *ptr, size_t blocks);
u16 pmm_refcount(void *ptr);

void reclaim_boot_map(e820_map_t *mmap);
