#pragma once

#include <base/addr.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>

#define PAGE_SHIFT 12

#define CANONICAL_MASK 0xffffffffffffUL
#define PHYSICAL_MASK  0x7fffffffffUL
#define FLAGS_MASK     0xfff

#define GET_CANONICAL(a) ((u64)(a) & CANONICAL_MASK)

#define IS_PAGE_ALIGNED(a) (!((u64)a % PAGE_4KIB))

// Intel manual Figure 5-23. (PDF page 604)
// https://web.archive.org/web/20230324001330/https://www.amd.com/system/files/TechDocs/40332.pdf
#define GET_LVL4_INDEX(addr) ((addr >> 39) & 0x1ff)
#define GET_LVL3_INDEX(addr) ((addr >> 30) & 0x1ff)
#define GET_LVL2_INDEX(addr) ((addr >> 21) & 0x1ff)
#define GET_LVL1_INDEX(addr) ((addr >> 12) & 0x1ff)

enum page_table_flags : u64 {
    PT_PRESENT = 1 << 0,
    PT_READ_WRITE = 1 << 1,
    PT_USER = 1 << 2,
    PT_WRITE_THROUGH = 1 << 3,
    PT_NO_CACHE = 1 << 4,
    PT_ACCESSED = 1 << 5,
    PT_DIRTY = 1 << 6,
    PT_HUGE = 1 << 7, // Only applies to level 2 or 3 pages
    PT_GLOBAL = 1 << 8,
    PT_NO_EXECUTE = 1ULL << 63,
};

typedef enum {
    PAGE_4KIB = 4 * KiB,
    PAGE_2MIB = 2 * MiB,
    PAGE_1GIB = 1 * GiB,
} page_size;

// https://wiki.osdev.org/File:64-bit_page_tables1.png
// Intel manual Figure 4-11. (PDF page 3136)
typedef union {
    struct PACKED {
        u64 present      : 1;
        u64 writable     : 1;
        u64 user         : 1;
        u64 writethrough : 1;
        u64 nocache      : 1;
        u64 accessed     : 1;
        u64 dirty        : 1;
        u64 huge         : 1;
        u64 global       : 1;
        u64 _free0       : 3;
        u64 addr         : 28; // 40 - 12
        u64 _reserved    : 12;
        u64 _free1       : 11;
        u64 nx           : 1;
    } bits;

    u64 raw;
} page_table;


bool supports_1gib_pages(void);

page_table* page_table_vaddr(page_table* parent);
