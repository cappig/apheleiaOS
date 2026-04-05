#pragma once

#include <arch/paging.h>
#include <base/types.h>

void map_page(page_t *root, size_t size, u64 vaddr, u64 paddr, u64 flags);
void unmap_page(page_t *root, u64 vaddr);

void map_region(page_t *root, size_t pages, u64 vaddr, u64 paddr, u64 flags);
size_t get_page(page_t *root, u64 vaddr, page_t **entry);
