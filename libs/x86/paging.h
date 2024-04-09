#pragma once

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>

#define PAGE_SHIFT 12

#define GET_PAGE_SIZE(p)   ((u64)(p) << PAGE_SHIFT)
#define IS_PAGE_ALIGNED(v) (((u64)v % PAGE_4KIB) == 0)
#define GET_PADDR(e)       ((u64)(e) & 0x0000FFFFFFFFF000)

// Figure 5-23. page 148 (PDF page 604)
// https://www.amd.com/system/files/TechDocs/40332.pdf
#define GET_LVL4_INDEX(addr) ((addr & ((u64)0x1ff << 39)) >> 39)
#define GET_LVL3_INDEX(addr) ((addr & ((u64)0x1ff << 30)) >> 30)
#define GET_LVL2_INDEX(addr) ((addr & ((u64)0x1ff << 21)) >> 21)
#define GET_LVL1_INDEX(addr) ((addr & ((u64)0x1ff << 12)) >> 12)

#define BASE_FLAGS (u64)(PT_PRESENT | PT_READ_WRITE)

enum page_table_flags : u64 {
    PT_PRESENT = 1 << 0,
    PT_READ_WRITE = 1 << 1,
    PT_USER = 1 << 2,
    PT_WRITE_THROUGH = 1 << 3,
    PT_NO_CACHE = 1 << 4,
    PT_ACCESSED = 1 << 5,
    PT_DIRTY = 1 << 6,
    PT_GLOBAL = 1 << 8,

    PT_HUGE = 1 << 7, // Only applies to level 2 or 3 pages
    PT_NO_EXECUTE = 1ULL << 63, // Only works if the NX bit is set
};

typedef enum {
    PAGE_4KIB = 4 * KiB,
    PAGE_2MIB = 2 * MiB,
    PAGE_1GIB = 1 * GiB,
} page_size;

typedef struct ALIGNED(PAGE_4KIB) {
    u64 entries[512];
} page_table;


bool supports_1gib_pages(void);
