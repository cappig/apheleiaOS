#include <base/macros.h>
#include <base/types.h>
#include <log/log.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "physical.h"
#include "virtual.h"
#include "x86/asm.h"
#include "x86/boot.h"

// Locate the requested index in the child table, allocate if it doesn't exist
static page_t *_walk_table_once(page_t *table, size_t index, u64 flags) {
    page_t *next_table;

    if (table[index] & PT_PRESENT) {
        next_table = (page_t *)(uintptr_t)page_get_paddr(&table[index]);
    } else {
        next_table = alloc_frames(1);
        memset(
            (void *)((uintptr_t)next_table + LINEAR_MAP_OFFSET_64), 0, PAGE_4KIB
        );

        page_set_paddr(&table[index], (u64)(uintptr_t)next_table);
        table[index] |= PT_PRESENT;
    }

    table[index] |= flags & FLAGS_MASK;
    table[index] |= PT_WRITE;
    table[index] &= ~PT_NO_EXECUTE;

    return (page_t *)((uintptr_t)next_table + LINEAR_MAP_OFFSET_64);
}

void map_page(
    page_t *lvl4_paddr,
    size_t size,
    u64 vaddr,
    u64 paddr,
    u64 flags
) {
    size_t lvl4_index = GET_LVL4_INDEX(vaddr);
    page_t *lvl4 = (page_t *)((uintptr_t)lvl4_paddr + LINEAR_MAP_OFFSET_64);

    size_t lvl3_index = GET_LVL3_INDEX(vaddr);
    page_t *lvl3 = _walk_table_once(lvl4, lvl4_index, flags);

    page_t *entry;

    if (size == PAGE_1GIB) {
        entry = &lvl3[lvl3_index];

        paddr = ALIGN_DOWN(paddr, PAGE_1GIB);
        flags |= PT_HUGE;
        goto finalize;
    }

    size_t lvl2_index = GET_LVL2_INDEX(vaddr);
    page_t *lvl2 = _walk_table_once(lvl3, lvl3_index, flags);

    if (size == PAGE_2MIB) {
        entry = &lvl2[lvl2_index];

        paddr = ALIGN_DOWN(paddr, PAGE_2MIB);
        flags |= PT_HUGE;
        goto finalize;
    }

    size_t lvl1_index = GET_LVL1_INDEX(vaddr);
    page_t *lvl1 = _walk_table_once(lvl2, lvl2_index, flags);

    entry = &lvl1[lvl1_index];

finalize:
    page_set_paddr(entry, paddr);

    flags |= PT_PRESENT; // Should this be assumed?
    *entry |= flags & FLAGS_MASK;
}

void unmap_page(page_t *lvl4_paddr, u64 vaddr) {
    page_t *page = NULL;

    get_page(lvl4_paddr, vaddr, &page);

    if (page) {
        *page = 0;
        tlb_flush(vaddr);
    }
}


void map_region(
    page_t *lvl4_paddr,
    size_t pages,
    u64 vaddr,
    u64 paddr,
    u64 flags
) {
    for (size_t i = 0; i < pages; i++) {
        u64 page_vaddr = vaddr + i * PAGE_4KIB;
        u64 page_paddr = paddr + i * PAGE_4KIB;

        map_page(lvl4_paddr, PAGE_4KIB, page_vaddr, page_paddr, flags);
    }
}

void identity_map(
    page_t *lvl4_paddr,
    u64 from,
    u64 to,
    u64 map_offset,
    u64 flags,
    bool remap
) {
    from = ALIGN_DOWN(from, PAGE_4KIB);
    to = ALIGN(to, PAGE_4KIB);

    // The bottom 4GiB are already mapped by the bootloader
    if (!remap) {
        from = max(PROTECTED_MODE_TOP, from);
    }

    // Map in a range [from, to>
    for (u64 i = from; i < to; i += PAGE_4KIB) {
        map_page(lvl4_paddr, PAGE_4KIB, i + map_offset, i, flags);
    }
}


size_t get_page(page_t *lvl4_paddr, u64 vaddr, page_t **entry) {
    size_t lvl4_index = GET_LVL4_INDEX(vaddr);
    page_t *lvl4 = (page_t *)((uintptr_t)lvl4_paddr + LINEAR_MAP_OFFSET_64);

    *entry = NULL;

    if (!lvl4) {
        return 0;
    }

    size_t lvl3_index = GET_LVL3_INDEX(vaddr);
    page_t *lvl3 = page_get_vaddr(&lvl4[lvl4_index]);

    if (lvl3[lvl3_index] & PT_HUGE && lvl3[lvl3_index] & PT_PRESENT) {
        *entry = &lvl3[lvl3_index];
        return PAGE_1GIB;
    }

    size_t lvl2_index = GET_LVL2_INDEX(vaddr);
    page_t *lvl2 = page_get_vaddr(&lvl3[lvl3_index]);

    if (lvl2[lvl2_index] & PT_HUGE && lvl2[lvl2_index] & PT_PRESENT) {
        *entry = &lvl2[lvl2_index];
        return PAGE_2MIB;
    }

    size_t lvl1_index = GET_LVL1_INDEX(vaddr);
    page_t *lvl1 = page_get_vaddr(&lvl2[lvl2_index]);

    if (lvl1[lvl1_index] & PT_PRESENT) {
        *entry = &lvl1[lvl1_index];
        return PAGE_4KIB;
    }

    return 0;
}
