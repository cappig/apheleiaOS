#pragma once

#include <x86/e820.h>


void pmm_init(e820_map_t* mmap);

size_t pmm_total_mem(void);
size_t pmm_free_mem(void);

void* alloc_frames(size_t count);
void free_frames(void* ptr, size_t size);

void reclaim_boot_map(e820_map_t* mmap);
