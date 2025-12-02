#pragma once

#include <base/types.h>
#include <stddef.h>


void identity_map_64(u64 top_address, u64 offset, bool is_kernel);

void map_page_64(size_t size, u64 vaddr, u64 paddr, u64 flags, bool is_kernel);
void map_region_64(size_t size, u64 vaddr, u64 paddr, u64 flags, bool is_kernel);

void setup_paging_64(void);
void init_paging_64(void);

u64 load_elf_sections_64(void* elf_file);
