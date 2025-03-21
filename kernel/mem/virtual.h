#pragma once

#include <x86/paging.h>

#include "arch/idt.h"


void map_page(page_table* lvl4_paddr, page_size size, u64 vaddr, u64 paddr, u64 flags);
void map_region(page_table* lvl4_paddr, usize pages, u64 vaddr, u64 paddr, u64 flags);
void identity_map(page_table* lvl4_paddr, u64 from, u64 to, u64 map_offset, u64 flags, bool remap);

void unmap_page(page_table* lvl4_paddr, u64 vaddr);

usize get_page(page_table* lvl4_paddr, u64 vaddr, page_table** entry);

page_table* clone_table(page_table* src_paddr);
void free_table(page_table* src_paddr);

void dump_table(page_table* src_paddr, bool kernel);
