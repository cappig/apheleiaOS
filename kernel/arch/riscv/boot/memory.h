#pragma once

#include <base/types.h>
#include <stdbool.h>
#include <stddef.h>

void boot_heap_init(uintptr_t start, uintptr_t end);
void *boot_alloc_aligned(size_t size, size_t align, bool zero);
