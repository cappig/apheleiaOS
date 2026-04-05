#pragma once

#include <base/macros.h>
#include <base/types.h>
#include <base/units.h>

#if __riscv_xlen == 64
typedef u64 page_t;

#define RISCV_PAGING_MODE 8ULL
#define RISCV_MMIO_BASE   0xffffffc000000000ULL
#define GET_LVL3_INDEX(addr) (((u64)(addr) >> 30) & 0x1ff)
#define GET_LVL2_INDEX(addr) (((u64)(addr) >> 21) & 0x1ff)
#define GET_LVL1_INDEX(addr) (((u64)(addr) >> 12) & 0x1ff)
#else
typedef u32 page_t;

#define RISCV_PAGING_MODE 1ULL
#define RISCV_MMIO_BASE   0xff000000UL
#define GET_LVL2_INDEX(addr) (((u32)(addr) >> 22) & 0x3ff)
#define GET_LVL1_INDEX(addr) (((u32)(addr) >> 12) & 0x3ff)
#endif

#define RISCV_KERNEL_BASE 0x80000000ULL

#define PAGE_4KIB (4 * KIB)
#define PAGE_2MIB (2 * MIB)

#define PT_PRESENT       (1ULL << 0)
#define PT_READ          (1ULL << 1)
#define PT_WRITE         (1ULL << 2)
#define PT_EXECUTE       (1ULL << 3)
#define PT_USER          (1ULL << 4)
#define PT_GLOBAL        (1ULL << 5)
#define PT_ACCESSED      (1ULL << 6)
#define PT_DIRTY         (1ULL << 7)

#if __riscv_xlen == 64
#define PT_WRITE_THROUGH (1ULL << 60)
#define PT_NO_CACHE      (1ULL << 61)
#define PT_HUGE          (1ULL << 62)
#define PT_NO_EXECUTE    (1ULL << 63)

#define FLAGS_MASK                                                        \
    (PT_PRESENT | PT_READ | PT_WRITE | PT_EXECUTE | PT_USER | PT_GLOBAL |  \
     PT_ACCESSED | PT_DIRTY | PT_WRITE_THROUGH | PT_NO_CACHE | PT_HUGE |   \
     PT_NO_EXECUTE)
#else
#define PT_WRITE_THROUGH 0ULL
#define PT_NO_CACHE      0ULL
#define PT_HUGE          0ULL
#define PT_NO_EXECUTE    (1ULL << 63)

#define FLAGS_MASK                                                   \
    (PT_PRESENT | PT_READ | PT_WRITE | PT_EXECUTE | PT_USER |        \
     PT_GLOBAL | PT_ACCESSED | PT_DIRTY)
#endif

static inline page_t page_get_paddr(page_t *page) {
    return ((*page >> 10) << 12);
}

static inline void page_set_paddr(page_t *page, page_t addr) {
    addr = ALIGN_DOWN(addr, PAGE_4KIB);
    *page = (addr >> 12) << 10;
}
