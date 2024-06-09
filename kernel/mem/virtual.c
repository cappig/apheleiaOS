#include "virtual.h"

#include <base/addr.h>
#include <string.h>
#include <x86/paging.h>

#include "base/macros.h"
#include "base/types.h"
#include "physical.h"


// Locate the requested index in the child table, allocate if it doesn't exist
static page_table* _walk_table_once(page_table* table, usize index) {
    page_table* next_table;

    if (table[index].bits.present) {
        next_table = (page_table*)(uptr)(table[index].bits.addr << PAGE_SHIFT);
    } else {
        next_table = alloc_frames(1);

        table[index].bits.present = 1;
        table[index].bits.writable = 1;
        table[index].bits.addr = (u64)next_table >> PAGE_SHIFT;
    }

    return (page_table*)ID_MAPPED_VADDR(next_table);
}

// NOTE: edge case: a child page is mapped and the overlapping parent gets mapped as huge to a
// different address. This creates a floating allocation because the reference to the child is lost
void map_page(page_table* lvl4_paddr, page_size size, u64 vaddr, u64 paddr, u64 flags) {
    usize lvl4_index = GET_LVL4_INDEX(vaddr);
    page_table* lvl4 = (page_table*)ID_MAPPED_VADDR(lvl4_paddr);

    usize lvl3_index = GET_LVL3_INDEX(vaddr);
    page_table* lvl3 = _walk_table_once(lvl4, lvl4_index);

    page_table* entry;

    if (size == PAGE_1GIB) {
        entry = &lvl3[lvl3_index];

        entry->bits.huge = 1;
        goto finalize;
    }

    usize lvl2_index = GET_LVL2_INDEX(vaddr);
    page_table* lvl2 = _walk_table_once(lvl3, lvl3_index);

    if (size == PAGE_2MIB) {
        entry = &lvl2[lvl2_index];

        entry->bits.huge = 1;
        goto finalize;
    }

    usize lvl1_index = GET_LVL1_INDEX(vaddr);
    page_table* lvl1 = _walk_table_once(lvl2, lvl2_index);

    entry = &lvl1[lvl1_index];

finalize:
    entry->raw |= flags;
    entry->bits.present = 1;

    paddr = ALIGN(paddr, size);
    entry->bits.addr = paddr >> PAGE_SHIFT;
}

void unmap_page(page_table* lvl4_paddr, u64 vaddr) {
    usize lvl4_index = GET_LVL4_INDEX(vaddr);
    page_table* lvl4 = (page_table*)ID_MAPPED_VADDR(lvl4_paddr);

    usize lvl3_index = GET_LVL3_INDEX(vaddr);
    page_table* lvl3 = page_table_vaddr(&lvl4[lvl4_index]);

    if (lvl3[lvl3_index].bits.huge) {
        lvl3[lvl3_index].raw = 0;
        return;
    }

    usize lvl2_index = GET_LVL2_INDEX(vaddr);
    page_table* lvl2 = page_table_vaddr(&lvl3[lvl3_index]);

    if (lvl2[lvl2_index].bits.huge) {
        lvl2[lvl2_index].raw = 0;
        return;
    }

    usize lvl1_index = GET_LVL1_INDEX(vaddr);
    page_table* lvl1 = page_table_vaddr(&lvl2[lvl2_index]);

    if (lvl1[lvl1_index].bits.present) {
        lvl1[lvl1_index].raw = 0;
    }
}


static inline void _clone_page_flags(page_table* src_vaddr, page_table* dest_vaddr) {
    u64 flags = src_vaddr->raw & FLAGS_MASK;
    dest_vaddr->raw |= flags;
}

static inline void _set_page_addr(page_table* dest_vaddr, u64 addr) {
    dest_vaddr->bits.present = 1;
    // dest_vaddr->bits.writable = 1;
    // dest_vaddr->bits.user = 1;

    dest_vaddr->bits.addr = addr;
}

static void _recursive_clone(page_table* src_vaddr, page_table* dest_vaddr, usize level) {
    page_table* new_paddr = alloc_frames(1);
    page_table* new_vaddr = (page_table*)ID_MAPPED_VADDR(new_paddr);

    memset((void*)new_vaddr, 0, 512 * sizeof(page_table));

    _clone_page_flags(src_vaddr, new_vaddr);
    _set_page_addr(dest_vaddr, (u64)new_paddr >> PAGE_SHIFT);

    page_table* next_vaddr = page_table_vaddr(src_vaddr);

    for (usize i = 0; i < 512; i++) {
        if (!next_vaddr[i].bits.present)
            continue;

        if (next_vaddr[i].bits.huge || level == 2)
            _set_page_addr(&new_vaddr[i], next_vaddr[i].bits.addr);
        else
            _recursive_clone(&next_vaddr[i], &new_vaddr[i], level - 1);
    }
}

page_table* clone_table(page_table* src_paddr) {
    page_table* src_vaddr = (page_table*)ID_MAPPED_VADDR(src_paddr);

    page_table* new_paddr = alloc_frames(1);
    page_table* new_vaddr = (page_table*)ID_MAPPED_VADDR(new_paddr);

    // The higher half contains kernel mappings.
    // They are always the same so we just have to clone the root level of the table
    // These addresses point to kernel level mappings that are always allocated
    memcpy(&new_vaddr[256], &src_vaddr[256], 256 * sizeof(u64));

    // The lower half is where the "fun" begins. We have to clone pages here.
    memset(new_vaddr, 0, 256 * sizeof(u64));

    // Root pages are never leafs
    for (usize i = 0; i < 256; i++)
        if (src_vaddr[i].bits.present)
            _recursive_clone(&src_vaddr[i], &new_vaddr[i], 3);

    return new_paddr;
}
