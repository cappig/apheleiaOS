#pragma once

#include <x86/paging.h>


void map_page(page_size size, u64 vaddr, u64 paddr, u64 flags);
void unmap_page(void* lvl4, page_size size, u64 vaddr);
