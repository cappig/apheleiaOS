#include "paging64.h"

#include <base/macros.h>
#include <base/types.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "memory.h"
#include "parse/elf.h"
#include "x86/lib/asm.h"
#include "x86/lib/paging64.h"

static page_t* lvl4;


static page_t* _walk_table_once(page_t* table, size_t index, bool is_kernel) {
    page_t* next_table;

    if (table[index] & PT_PRESENT) {
        next_table = (page_t*)(uintptr_t)page_get_paddr(&table[index]);
    } else {
        u32 type = is_kernel ? E820_KERNEL : E820_PAGE_TABLE;
        next_table = (page_t*)mmap_alloc(PAGE_4KIB, type, PAGE_4KIB);

        page_set_paddr(&table[index], (u64)(uintptr_t)next_table);

        table[index] |= PT_PRESENT;
        table[index] |= PT_WRITE;
    }

    return next_table;
}

void map_page_64(size_t size, u64 vaddr, u64 paddr, u64 flags, bool is_kernel) {
    size_t lvl4_index = GET_LVL4_INDEX(vaddr);

    size_t lvl3_index = GET_LVL3_INDEX(vaddr);
    page_t* lvl3 = _walk_table_once(lvl4, lvl4_index, is_kernel);

    page_t* entry;

    if (size == PAGE_1GIB) {
        entry = &lvl3[lvl3_index];

        paddr = ALIGN_DOWN(paddr, PAGE_1GIB);
        flags |= PT_HUGE;
        goto finalize;
    }

    size_t lvl2_index = GET_LVL2_INDEX(vaddr);
    page_t* lvl2 = _walk_table_once(lvl3, lvl3_index, is_kernel);

    if (size == PAGE_2MIB) {
        entry = &lvl2[lvl2_index];

        paddr = ALIGN_DOWN(paddr, PAGE_2MIB);
        flags |= PT_HUGE;
        goto finalize;
    }

    size_t lvl1_index = GET_LVL1_INDEX(vaddr);
    page_t* lvl1 = _walk_table_once(lvl2, lvl2_index, is_kernel);

    entry = &lvl1[lvl1_index];

finalize:
    page_set_paddr(entry, paddr);

    flags |= PT_PRESENT;
    *entry |= flags & FLAGS_MASK;
}

// TODO: we should try to use larger pages if possible
void map_region_64(size_t size, u64 vaddr, u64 paddr, u64 flags, bool is_kernel) {
    for (size_t i = 0; i < DIV_ROUND_UP(size, PAGE_4KIB); i++) {
        u64 page_vaddr = vaddr + i * PAGE_4KIB;
        u64 page_paddr = paddr + i * PAGE_4KIB;

        map_page_64(PAGE_4KIB, page_vaddr, page_paddr, flags, is_kernel);
    }
}

// Should we be assuming the existence of huge pages?
void identity_map_64(u64 top_address, u64 offset, bool is_kernel) {
    for (u64 i = 0; i < top_address; i += PAGE_2MIB)
        map_page_64(PAGE_2MIB, i + offset, i, PT_WRITE, is_kernel);
}

void setup_paging_64(void) {
    // Allocate the root table
    lvl4 = (page_t*)mmap_alloc(PAGE_4KIB, E820_KERNEL, PAGE_4KIB);
    write_cr3((u32)(uintptr_t)lvl4);

    // Enable the NX bit
    u64 efer = read_msr(EFER_MSR);
    write_msr(EFER_MSR, efer | EFER_NX);

    // Enable write protect
    u32 cr0 = read_cr0();
    write_cr0(cr0 | CR0_WP);
}

void init_paging_64(void) {
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


static page_t _elf_to_page_flags(u32 elf_flags) {
    u64 flags = PT_PRESENT;

    if (elf_flags & PF_W)
        flags |= PT_WRITE;

    if (!(elf_flags & PF_X))
        flags |= PT_NO_EXECUTE;

    return flags;
}

u64 load_elf_sections_64(void* elf_file) {
    elf_header_t* header = elf_file;

    for (size_t i = 0; i < header->ph_num; i++) {
        elf_prog_header_t* p_header = elf_file + header->phoff + i * header->phent_size;

        if (p_header->type != PT_LOAD)
            continue;

        if (!p_header->file_size && !p_header->mem_size)
            continue;

        u64 size = ALIGN(p_header->mem_size, PAGE_4KIB);

        u64 flags = _elf_to_page_flags(p_header->flags);

        u64 pbase = (u64)(uintptr_t)mmap_alloc(size, E820_KERNEL, p_header->align);
        u64 vbase = p_header->vaddr;

        // Map the segment
        map_region_64(size, vbase, pbase, flags, true);

        // Copy all loadable data from the file
        memcpy((void*)(uintptr_t)pbase, elf_file + p_header->offset, p_header->file_size);

        // Zero out any additional space
        size_t zero_len = p_header->mem_size - p_header->file_size;
        memset((void*)(uintptr_t)pbase + p_header->file_size, 0, zero_len);
    }

    return header->entry;
}
