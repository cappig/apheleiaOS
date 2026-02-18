#pragma once

#include <base/types.h>
#include <stddef.h>

#if defined(__x86_64__)
#include "x86/paging64.h"
#else
#include "x86/paging32.h"
#endif

void map_page(page_t *lvl4_paddr, size_t size, u64 vaddr, u64 paddr, u64 flags);
void unmap_page(page_t *lvl4_paddr, u64 vaddr);

void map_region(page_t *lvl4_paddr, size_t pages, u64 vaddr, u64 paddr, u64 flags);
void identity_map(page_t *lvl4_paddr, u64 from, u64 to, u64 map_offset, u64 flags, bool remap);

size_t get_page(page_t *lvl4_paddr, u64 vaddr, page_t **entry);
