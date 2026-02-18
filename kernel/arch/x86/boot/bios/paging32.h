#pragma once

#include <base/types.h>
#include <stddef.h>


void map_page_32(size_t size, u32 vaddr, u64 paddr, u64 flags, bool is_kernel);
void map_region_32(size_t size, u32 vaddr, u64 paddr, u64 flags, bool is_kernel);
void identity_map_32(u32 top_address, u32 offset, bool is_kernel);

void setup_paging_32(void);
void init_paging_32(void);

u32 load_elf_sections_32(void *elf_file);
