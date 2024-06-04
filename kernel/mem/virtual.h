#pragma once

#include <x86/paging.h>


void map_page(page_table* lvl4_paddr, page_size size, u64 vaddr, u64 paddr, u64 flags);
void unmap_page(page_table* lvl4_paddr, u64 vaddr);

page_table* clone_table(page_table* src_paddr);
