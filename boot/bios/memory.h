#pragma once

#include <base/attributes.h>
#include <stddef.h>
#include <x86/e820.h>


void get_e820(e820_map* mmap);

void* mmap_alloc(usize bytes, u32 type, uptr top);

void* boot_malloc(size_t size) ATTRIBUTE(malloc);
void* boot_malloc_low(size_t size) ATTRIBUTE(malloc);
void* boot_calloc(size_t size) ATTRIBUTE(malloc);

void boot_free(void* ptr);
