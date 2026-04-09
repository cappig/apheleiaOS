#pragma once

#include <arch/paging.h>
#include <base/types.h>

static inline page_t pte_leaf_flags(u64 flags) {
    return PT_PRESENT | PT_ACCESSED | PT_DIRTY | PT_READ
        | (flags & PT_WRITE)
        | (flags & PT_USER)
        | (flags & PT_GLOBAL)
        | ((flags & PT_NO_EXECUTE) ? 0 : PT_EXECUTE);
}

void map_page(page_t *root, u64 vaddr, u64 paddr, u64 flags);
void unmap_page(page_t *root, u64 vaddr);

void map_region(page_t *root, size_t pages, u64 vaddr, u64 paddr, u64 flags);
size_t get_page(page_t *root, u64 vaddr, page_t **entry);
