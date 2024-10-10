#pragma once

#include <base/types.h>
#include <x86/paging.h>


void identity_map(u64 top_adress, u64 offset, bool is_kernel);

void map_page(page_size size, u64 vaddr, u64 paddr, u64 flags, bool is_kernel);
void map_region(usize size, u64 vaddr, u64 paddr, u64 flags, bool is_kernel);

void setup_paging(void);
void init_paging(void);
