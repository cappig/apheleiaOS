#pragma once

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <stddef.h>


#if defined(__x86_64__)
typedef u64 page_t;

#define FLAGS_MASK        0x8000000000000fffULL
#define ADDR_MASK         0x000ffffffffff000ULL
#define PHYSICAL_MASK     (1ULL << 52)
#define LINEAR_MAP_OFFSET 0xffff888000000000ULL

#define GET_LVL4_INDEX(addr) (((addr) >> 39) & 0x1ff)
#define GET_LVL3_INDEX(addr) (((addr) >> 30) & 0x1ff)
#define GET_LVL2_INDEX(addr) (((addr) >> 21) & 0x1ff)
#define GET_LVL1_INDEX(addr) (((addr) >> 12) & 0x1ff)

#else
typedef u32 page_t;

#define FLAGS_MASK        0x00000fffUL
#define ADDR_MASK         0xfffff000UL
#define PHYSICAL_MASK     (1ULL << 32)
#define LINEAR_MAP_OFFSET 0xc0000000UL

#define GET_LVL2_INDEX(addr) (((addr) >> 22) & 0x3ff)
#define GET_LVL1_INDEX(addr) (((addr) >> 12) & 0x3ff)
#endif

#define PROTECTED_MODE_TOP 0x100000000UL

#define PT_PRESENT       (1 << 0)
#define PT_WRITE         (1 << 1)
#define PT_USER          (1 << 2)
#define PT_WRITE_THROUGH (1 << 3)
#define PT_NO_CACHE      (1 << 4)
#define PT_ACCESSED      (1 << 5)
#define PT_DIRTY         (1 << 6)
#define PT_HUGE          (1 << 7)
#define PT_GLOBAL        (1 << 8)
#if defined(__x86_64__)
#define PT_NO_EXECUTE (1ULL << 63)
#endif

#define PAGE_4KIB (4 * KIB)
#if defined(__x86_64__)
#define PAGE_2MIB (2 * MIB)
#define PAGE_1GIB (1 * GIB)
#else
#define PAGE_4MIB (4 * MIB)
#endif

#define PF_PRESENT  (1 << 0)
#define PF_WRITE    (1 << 1)
#define PF_USER     (1 << 2)
#define PF_RESERVED (1 << 3)
#define PF_FETCH    (1 << 4)
#define PF_KEY      (1 << 5)
#define PF_SSTACK   (1 << 6)
#define PF_SGX      (1 << 15)

inline page_t page_get_paddr(page_t* page) {
    page_t ret = *page & ADDR_MASK;
    return ret % PHYSICAL_MASK;
}

inline void* page_get_vaddr(page_t* page) {
    page_t paddr = page_get_paddr(page);
    return (void*)(uintptr_t)(paddr + LINEAR_MAP_OFFSET);
}

inline void page_set_paddr(page_t* page, page_t addr) {
    addr = ALIGN_DOWN(addr, PAGE_4KIB);
    addr %= PHYSICAL_MASK;
    *page = addr;
}

#if defined(__x86_64__)
inline page_t construct_vaddr(size_t lvl4, size_t lvl3, size_t lvl2, size_t lvl1) {
    page_t addr = 0;
    addr |= ((page_t)(lvl1 & 0x1ff) << 12);
    addr |= ((page_t)(lvl2 & 0x1ff) << 21);
    addr |= ((page_t)(lvl3 & 0x1ff) << 30);
    addr |= ((page_t)(lvl4 & 0x1ff) << 39);
    return addr;
}
#else
inline page_t construct_vaddr(size_t lvl2, size_t lvl1) {
    page_t addr = 0;
    addr |= ((page_t)(lvl1 & 0x3ff) << 12);
    addr |= ((page_t)(lvl2 & 0x3ff) << 22);
    return addr;
}
#endif
