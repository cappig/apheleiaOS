#include "paging.h"

#include <base/macros.h>
#include <base/types.h>
#include <x86/asm.h>
#include <x86/e820.h>
#include <x86/paging.h>

#include "memory.h"

static page_table* lvl4;


static page_table* _walk_table_once(page_table* table, usize index, bool is_kernel) {
    page_table* next_table;

    if (table[index].bits.present) {
        next_table = (page_table*)(uptr)(table[index].bits.addr << PAGE_SHIFT);
    } else {
        u32 type = is_kernel ? E820_KERNEL : E820_PAGE_TABLE;
        next_table = (page_table*)mmap_alloc(PAGE_4KIB, type, PAGE_4KIB);

        table[index].bits.present = 1;
        table[index].bits.writable = 1;
        table[index].bits.addr = (u64)(uptr)next_table >> PAGE_SHIFT;
    }

    return next_table;
}

void map_page(page_size size, u64 vaddr, u64 paddr, u64 flags, bool is_kernel) {
    usize lvl4_index = GET_LVL4_INDEX(vaddr);

    usize lvl3_index = GET_LVL3_INDEX(vaddr);
    page_table* lvl3 = _walk_table_once(lvl4, lvl4_index, is_kernel);

    page_table* entry;

    if (size == PAGE_1GIB) {
        entry = &lvl3[lvl3_index];

        entry->bits.huge = 1;
        goto finalize;
    }

    usize lvl2_index = GET_LVL2_INDEX(vaddr);
    page_table* lvl2 = _walk_table_once(lvl3, lvl3_index, is_kernel);

    if (size == PAGE_2MIB) {
        entry = &lvl2[lvl2_index];

        entry->bits.huge = 1;
        goto finalize;
    }

    usize lvl1_index = GET_LVL1_INDEX(vaddr);
    page_table* lvl1 = _walk_table_once(lvl2, lvl2_index, is_kernel);

    entry = &lvl1[lvl1_index];

finalize:
    entry->raw |= flags;
    entry->bits.present = 1;

    paddr = ALIGN(paddr, size);
    entry->bits.addr = paddr >> PAGE_SHIFT;
}

// TODO: we should try to use larger pages if possible
void map_region(usize size, u64 vaddr, u64 paddr, u64 flags, bool is_kernel) {
    for (usize i = 0; i < DIV_ROUND_UP(size, PAGE_4KIB); i++) {
        u64 page_vaddr = vaddr + i * PAGE_4KIB;
        u64 page_paddr = paddr + i * PAGE_4KIB;

        map_page(PAGE_4KIB, page_vaddr, page_paddr, flags, is_kernel);
    }
}

void identity_map(u64 top_adress, u64 offset, bool is_kernel) {
    // TODO: make use of larger page sizes
    for (u64 i = 0; i <= top_adress; i += PAGE_2MIB)
        map_page(PAGE_2MIB, i + offset, i, PT_READ_WRITE, is_kernel);
}

void setup_paging(void) {
    // Allocate the root table
    lvl4 = (page_table*)mmap_alloc(PAGE_4KIB, E820_KERNEL, PAGE_4KIB);
    write_cr3((u32)(uptr)lvl4);

    // Enable the NX bit
    u64 efer = read_msr(EFER_MSR);
    write_msr(EFER_MSR, efer | EFER_NX);

    // Enable write protect
    u32 cr0 = read_cr0();
    write_cr0(cr0 | CR0_WP);
}

void init_paging(void) {
    // Enable Physical Address Extension
    u32 cr4 = read_cr4();
    write_cr4(cr4 | CR4_PAE);

    // Set the long mode bit
    u64 efer = read_msr(EFER_MSR);
    write_msr(EFER_MSR, efer | EFER_LME);

    // Set the paging bit in cr0
    u32 cr0 = read_cr0();
    write_cr0(cr0 | CR0_PG);
}
