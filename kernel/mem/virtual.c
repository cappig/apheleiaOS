#include "virtual.h"

#include <base/addr.h>
#include <base/macros.h>
#include <base/types.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <x86/paging.h>

#include "mem/physical.h"


// Locate the requested index in the child table, allocate if it doesn't exist
static page_table* _walk_table_once(page_table* table, usize index) {
    page_table* next_table;

    if (table[index].bits.present) {
        next_table = (page_table*)page_get_paddr(&table[index]);
    } else {
        next_table = alloc_frames(1);

        page_set_paddr(&table[index], (u64)next_table);

        table[index].bits.present = 1;
        table[index].bits.writable = 1;
    }

    return (page_table*)ID_MAPPED_VADDR(next_table);
}

// NOTE: edge case: a child page is mapped and the overlapping parent gets mapped as huge to a
// different address. This creates a floating allocation because the reference to the child is lost
void map_page(page_table* lvl4_paddr, page_size size, u64 vaddr, u64 paddr, u64 flags) {
    vaddr &= CANONICAL_MASK;
    paddr %= PHYSICAL_MASK;

    usize lvl4_index = GET_LVL4_INDEX(vaddr);
    page_table* lvl4 = (page_table*)ID_MAPPED_VADDR(lvl4_paddr);

    usize lvl3_index = GET_LVL3_INDEX(vaddr);
    page_table* lvl3 = _walk_table_once(lvl4, lvl4_index);

    page_table* entry;

    if (size == PAGE_1GIB) {
        entry = &lvl3[lvl3_index];

        paddr = ALIGN_DOWN(paddr, PAGE_1GIB);
        flags |= PT_HUGE;
        goto finalize;
    }

    usize lvl2_index = GET_LVL2_INDEX(vaddr);
    page_table* lvl2 = _walk_table_once(lvl3, lvl3_index);

    if (size == PAGE_2MIB) {
        entry = &lvl2[lvl2_index];

        paddr = ALIGN_DOWN(paddr, PAGE_2MIB);
        flags |= PT_HUGE;
        goto finalize;
    }

    usize lvl1_index = GET_LVL1_INDEX(vaddr);
    page_table* lvl1 = _walk_table_once(lvl2, lvl2_index);

    entry = &lvl1[lvl1_index];

finalize:
    page_set_paddr(entry, paddr);

    entry->raw |= flags;

#ifdef MMU_DEBUG
    log_debug(
        "[MMU DEBUG] mapped virtual page (cr3: %#lx): size = %#x, paddr = %#lx, vaddr = %#lx",
        (u64)lvl4_paddr,
        size,
        paddr,
        vaddr
    );
#endif
}

void map_region(page_table* lvl4_paddr, usize size, u64 vaddr, u64 paddr, u64 flags) {
    for (usize i = 0; i < DIV_ROUND_UP(size, PAGE_4KIB); i++) {
        u64 page_vaddr = vaddr + i * PAGE_4KIB;
        u64 page_paddr = paddr + i * PAGE_4KIB;

        map_page(lvl4_paddr, PAGE_4KIB, page_vaddr, page_paddr, flags);
    }
}

void identity_map(page_table* lvl4_paddr, u64 from, u64 to, u64 map_offset, u64 flags, bool remap) {
    from = ALIGN_DOWN(from, PAGE_4KIB);
    to = ALIGN(to, PAGE_4KIB);

    // The bottom 4GiB are already mapped by the bootloader
    if (!remap)
        from = max(PROTECTED_MODE_TOP, from);

    for (u64 i = from; i <= to; i += PAGE_4KIB)
        map_page(lvl4_paddr, PAGE_4KIB, i + map_offset, i, flags);
}

void unmap_page(page_table* lvl4_paddr, u64 vaddr) {
    usize lvl4_index = GET_LVL4_INDEX(vaddr);
    page_table* lvl4 = (page_table*)ID_MAPPED_VADDR(lvl4_paddr);

    usize lvl3_index = GET_LVL3_INDEX(vaddr);
    page_table* lvl3 = page_get_vaddr(&lvl4[lvl4_index]);

    MAYBE_UNUSED page_size size = 0;

    if (lvl3[lvl3_index].bits.huge && lvl3[lvl3_index].bits.present) {
        lvl3[lvl3_index].raw = 0;
        size = PAGE_1GIB;
        goto finalize;
    }

    usize lvl2_index = GET_LVL2_INDEX(vaddr);
    page_table* lvl2 = page_get_vaddr(&lvl3[lvl3_index]);

    if (lvl2[lvl2_index].bits.huge && lvl2[lvl2_index].bits.present) {
        lvl2[lvl2_index].raw = 0;
        size = PAGE_2MIB;
        goto finalize;
    }

    usize lvl1_index = GET_LVL1_INDEX(vaddr);
    page_table* lvl1 = page_get_vaddr(&lvl2[lvl2_index]);

    if (lvl1[lvl1_index].bits.present) {
        lvl1[lvl1_index].raw = 0;
        size = PAGE_4KIB;
    }

finalize:
#ifdef MMU_DEBUG
    log_debug(
        "[MMU DEBUG] unmapped virtual page (cr3: %#lx): size = %x, vaddr = %#lx",
        (u64)lvl4_paddr,
        size,
        vaddr
    );
#endif
}


static void _recursive_clone(page_table* src_vaddr, page_table* dest_vaddr, usize level) {
    page_table* new_paddr = alloc_frames(1);
    page_table* new_vaddr = (page_table*)ID_MAPPED_VADDR(new_paddr);

    memset((void*)new_vaddr, 0, 512 * sizeof(page_table));

    page_set_paddr(dest_vaddr, (u64)new_paddr);

    page_table* next_vaddr = page_get_vaddr(src_vaddr);

    for (usize i = 0; i < 512; i++) {
        if (!next_vaddr[i].bits.present)
            continue;

        if (next_vaddr[i].bits.huge || level == 1)
            new_vaddr[i].raw = next_vaddr[i].raw;
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

    // Root pages are never leaves
    for (usize i = 0; i < 256; i++)
        if (src_vaddr[i].bits.present)
            _recursive_clone(&src_vaddr[i], &new_vaddr[i], 3);

    return new_paddr;
}


// In order to free a page we have to make sure that it doesn't point to any lower level children
static void _recursive_free(page_table* src_vaddr, usize level) {
    u64 paddr = page_get_paddr(src_vaddr);
    page_table* vaddr = (page_table*)ID_MAPPED_VADDR(paddr);

    // A level 1 page can't have children, skip this whole check
    for (usize i = 0; i < 512 && level != 1; i++) {
        if (!vaddr[i].bits.present)
            continue;

        // Free any children, that is follow any pointers to lower level tables
        if (!vaddr[i].bits.huge)
            _recursive_free(&vaddr[i], level - 1);
    }

    // At this point the page has no children
    // This means that pointers will not be left dangling
    free_frames((void*)paddr, 1);
}

void free_table(page_table* src_paddr) {
    page_table* src_vaddr = (page_table*)ID_MAPPED_VADDR(src_paddr);

    // We have to walk the table to the lowest leaf and free all the frames that back valid pages
    for (usize i = 0; i < 256; i++)
        if (src_vaddr[i].bits.present)
            _recursive_free(&src_vaddr[i], 3);

    free_frames(src_paddr, 1);
}
