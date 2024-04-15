#pragma once

#include <x86/paging.h>


void vmm_init(void);

void map_page(page_size size, u64 vaddr, u64 paddr, u64 flags);
void unmap_page(u64 vaddr, bool free);
