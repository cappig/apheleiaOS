#pragma once

#include <x86/e820.h>


void get_e820(e820_map_t* mmap);

void get_rsdp(u64* rsdp);

void* mmap_alloc(size_t size, int type, size_t alignment);
void* mmap_alloc_top(size_t size, int type, size_t alignment, u64 top);

void arch_init_alloc(void);
