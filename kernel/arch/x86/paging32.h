#pragma once

#include "boot.h"

#ifndef PAGING_32_INCLUDED
#define PAGING_32_INCLUDED 1
#endif

#ifdef PAGING_64_INCLUDED
#error "Cannot include paging32.h and paging64.h in the same translation unit"
#endif

#include <base/macros.h>
#include <base/types.h>
#include <base/units.h>
#include <stddef.h>

typedef u64 page_t;

#define FLAGS_MASK    0x8000000000000fffULL
#define ADDR_MASK     0x000ffffffffff000ULL
#define PHYSICAL_MASK (1ULL << 36)

#define GET_LVL3_INDEX(addr) (((addr) >> 30) & 0x3)
#define GET_LVL2_INDEX(addr) (((addr) >> 21) & 0x1ff)
#define GET_LVL1_INDEX(addr) (((addr) >> 12) & 0x1ff)

#define PAGE_4KIB (4 * KIB)
#define PAGE_2MIB (2 * MIB)

#define PT_PRESENT       (1 << 0)
#define PT_WRITE         (1 << 1)
#define PT_USER          (1 << 2)
#define PT_WRITE_THROUGH (1 << 3)
#define PT_NO_CACHE      (1 << 4)
#define PT_ACCESSED      (1 << 5)
#define PT_DIRTY         (1 << 6)
#define PT_HUGE          (1 << 7)
#define PT_GLOBAL        (1 << 8)
#define PT_NO_EXECUTE    (1ULL << 63)

static inline page_t page_get_paddr(page_t* page) {
    return *page & ADDR_MASK;
}

static inline void* page_get_vaddr(page_t* page) {
    page_t paddr = page_get_paddr(page);
    return (void*)(uintptr_t)paddr;
}

static inline void page_set_paddr(page_t* page, page_t addr) {
    addr = ALIGN_DOWN(addr, PAGE_4KIB);
    addr &= ADDR_MASK;
    *page = addr;
}

static inline page_t construct_vaddr(size_t lvl3, size_t lvl2, size_t lvl1) {
    page_t addr = 0;
    addr |= ((page_t)(lvl1 & 0x1ff) << 12);
    addr |= ((page_t)(lvl2 & 0x1ff) << 21);
    addr |= ((page_t)(lvl3 & 0x3) << 30);
    return addr;
}
