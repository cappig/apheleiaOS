#pragma once

#include <base/attributes.h>
#include <x86/e820.h>


void get_e820(e820_map* mmap);

u64 alloc_kernel_stack(usize size);

void* mmap_alloc(usize bytes, u32 type, u32 alignment);

void* bmalloc_aligned(usize size, u32 alignment, bool allow_high);
void* bmalloc(usize size, bool allow_high);

void bfree(void* ptr);

u64 get_rsdp(void);
