#pragma once

#include <x86/e820.h>


void pmm_init(e820_map* mmap);

void* alloc_frames(usize count);
void free_frames(void* ptr, usize size);

void reclaim_boot_map(e820_map* mmap);
