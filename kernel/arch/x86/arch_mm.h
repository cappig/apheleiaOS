#pragma once

// x86 memory management wrappers
// Provides arch_* inline wrappers around the x86 PMM and VMM

#include <arch/paging.h>
#include <base/types.h>
#include <x86/mm/physical.h>
#include <x86/mm/virtual.h>

static inline void *arch_alloc_frames_user(size_t count) {
    return alloc_frames_user(count);
}

static inline void arch_free_frames(void *ptr, size_t count) {
    free_frames(ptr, count);
}

static inline void arch_map_region(void *root, size_t pages, u64 vaddr, u64 paddr, u64 flags) {
    map_region(root, pages, vaddr, paddr, flags);
}

static inline size_t arch_get_page(void *root, u64 vaddr, page_t **entry) {
    return get_page(root, vaddr, entry);
}

static inline u64 arch_page_get_paddr(page_t *entry) {
    return page_get_paddr(entry);
}

static inline void arch_page_set_paddr(page_t *entry, u64 paddr) {
    page_set_paddr(entry, paddr);
}
