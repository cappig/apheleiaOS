#pragma once

#include <base/attributes.h>
#include <x86/e820.h>


void get_e820(e820_map* mmap);

void* mmap_alloc(usize bytes, u32 type, u32 alignment);
void* bmalloc(usize size, bool allow_high) ATTRIBUTE(malloc);
void* bmalloc_aligned(usize size, u32 alignment, bool allow_high) ATTRIBUTE(malloc);

void bfree(void* ptr);
