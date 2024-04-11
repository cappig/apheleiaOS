#include <base/addr.h>
#include <x86/paging.h>

#include "physical.h"

#define GET_MAPPED_PADDR(paddr) (page_table*)(uptr)(ID_MAPPED_VADDR(GET_PADDR(paddr)))

static page_table* lvl4;


static page_table* _walk_table_once(page_table* table, usize index) {
    page_table* next_table;

    if ((u64)table->entries[index] & PT_PRESENT) {
        next_table = (page_table*)(uptr)GET_PADDR(table->entries[index]);
    } else {
        next_table = alloc_frames(1);

        table->entries[index] = (u64)(uptr)next_table | BASE_FLAGS;
    }

    return next_table;
}

void map_page(page_size size, u64 vaddr, u64 paddr, u64 flags) {
    usize lvl4_index = GET_LVL4_INDEX(vaddr);

    usize lvl3_index = GET_LVL3_INDEX(vaddr);
    page_table* lvl3 = _walk_table_once(lvl4, lvl4_index);

    if (size == PAGE_1GIB) {
        lvl3->entries[lvl3_index] = paddr | PT_PRESENT | PT_HUGE | flags;
        return;
    }

    usize lvl2_index = GET_LVL2_INDEX(vaddr);
    page_table* lvl2 = _walk_table_once(lvl3, lvl3_index);

    if (size == PAGE_2MIB) {
        lvl2->entries[lvl2_index] = paddr | PT_PRESENT | PT_HUGE | flags;
        return;
    }

    usize lvl1_index = GET_LVL1_INDEX(vaddr);
    page_table* lvl1 = _walk_table_once(lvl2, lvl2_index);

    lvl1->entries[lvl1_index] = paddr | PT_PRESENT | flags;
}


void unmap_page(page_size size, u64 vaddr) {
    usize lvl4_index = GET_LVL4_INDEX(vaddr);
    page_table* lvl4_vaddr = (page_table*)ID_MAPPED_VADDR(lvl4);

    usize lvl3_index = GET_LVL3_INDEX(vaddr);
    page_table* lvl3 = GET_MAPPED_PADDR(lvl4_vaddr->entries[lvl4_index]);

    if (size == PAGE_1GIB) {
        free_frames(&lvl3->entries[lvl3_index], PAGE_1GIB);
        lvl3->entries[lvl3_index] = 0;
        return;
    }

    usize lvl2_index = GET_LVL2_INDEX(vaddr);
    page_table* lvl2 = GET_MAPPED_PADDR(lvl3->entries[lvl3_index]);

    if (size == PAGE_2MIB) {
        free_frames(&lvl2->entries[lvl2_index], PAGE_2MIB);
        lvl2->entries[lvl2_index] = 0;
        return;
    }

    usize lvl1_index = GET_LVL1_INDEX(vaddr);
    page_table* lvl1 = GET_MAPPED_PADDR(lvl2->entries[lvl2_index]);

    if (size == PAGE_4KIB) {
        free_frames(&lvl1->entries[lvl1_index], PAGE_4KIB);
        lvl1->entries[lvl1_index] = 0;
    }
}
