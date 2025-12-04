#include "paging32.h"

#include <base/macros.h>
#include <base/types.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "memory.h"
#include "parse/elf.h"
#include "x86/asm.h"
#include "x86/paging32.h"

static page_t* pdir;

static page_t* _walk_table_once_32(page_t* table, size_t index, bool is_kernel) {
    page_t* next_table;

    if (table[index] & PT_PRESENT) {
        /* stored paddr -> convert to pointer */
        next_table = (page_t*)(uintptr_t)page_get_paddr(&table[index]);
    } else {
        u32 type = is_kernel ? E820_KERNEL : E820_PAGE_TABLE;
        next_table = (page_t*)mmap_alloc(PAGE_4KIB, type, PAGE_4KIB);

        /* record physical address into directory entry */
        page_set_paddr(&table[index], (page_t)(uintptr_t)next_table);

        table[index] |= PT_PRESENT;
        table[index] |= PT_WRITE;
    }

    return next_table;
}

/* size must be either PAGE_4KIB or PAGE_4MIB (PAGE_4MIB is "huge" 4 MiB page on 32-bit) */
void map_page_32(size_t size, u32 vaddr, u32 paddr, u32 flags, bool is_kernel) {
    size_t lvl2_index = GET_LVL2_INDEX(vaddr);
    size_t lvl1_index = GET_LVL1_INDEX(vaddr);

    /* if mapping a 4 MiB page -> set directory entry with PT_HUGE */
    if (size == PAGE_4MIB) {
        page_t* dir_entry = &pdir[lvl2_index];

        paddr = ALIGN_DOWN(paddr, PAGE_4MIB);
        page_set_paddr(dir_entry, (page_t)paddr);

        flags |= PT_HUGE;
        flags |= PT_PRESENT;
        *dir_entry |= flags & FLAGS_MASK;
        return;
    }

    /* otherwise, ensure page table exists and set 4 KiB page entry */
    page_t* pt = _walk_table_once_32(pdir, lvl2_index, is_kernel);

    page_t* entry = &pt[lvl1_index];

    page_set_paddr(entry, (page_t)paddr);

    flags |= PT_PRESENT;
    *entry |= flags & FLAGS_MASK;
}

/* Map region by breaking into 4 KiB pages */
void map_region_32(size_t size, u32 vaddr, u32 paddr, u32 flags, bool is_kernel) {
    for (size_t i = 0; i < DIV_ROUND_UP(size, PAGE_4KIB); i++) {
        u32 page_vaddr = vaddr + (u32)(i * PAGE_4KIB);
        u32 page_paddr = paddr + (u32)(i * PAGE_4KIB);

        map_page_32(PAGE_4KIB, page_vaddr, page_paddr, flags, is_kernel);
    }
}

/* Identity-map physical memory up to top_address. Use 4 MiB pages where possible. */
void identity_map_32(u32 top_address, u32 offset, bool is_kernel) {
    /* prefer 4 MiB steps for speed */
    u32 i = 0;
    while (i + PAGE_4MIB <= top_address) {
        map_page_32(PAGE_4MIB, i + offset, i, PT_WRITE, is_kernel);
        i += PAGE_4MIB;
    }

    /* remaining tail with 4 KiB pages */
    for (; i < top_address; i += PAGE_4KIB) {
        map_page_32(PAGE_4KIB, i + offset, i, PT_WRITE, is_kernel);
    }
}

void setup_paging_32(void) {
    /* Allocate the page directory (root) */
    pdir = (page_t*)mmap_alloc(PAGE_4KIB, E820_KERNEL, PAGE_4KIB);

    /* write physical base into CR3 */
    write_cr3((u32)(uintptr_t)pdir);

    /* Note: no NX config here (non-PAE 32-bit typically doesn't support NX).
       If you need NX on 32-bit, you must enable PAE and use appropriate MSRs. */
}

void init_paging_32(void) {
    /* Enable Page Size Extension (PSE) if you want to use 4 MiB pages.
       CR4_PSE should be defined in your headers (commonly (1 << 4)). */
    // #ifdef CR4_PSE
    //     u32 cr4 = read_cr4();
    //     write_cr4(cr4 | CR4_PSE);
    // #endif

    /* Enable paging by setting CR0.PG */
    u32 cr0 = read_cr0();
    write_cr0(cr0 | CR0_PG);
}


static page_t _elf_to_page_flags(u32 elf_flags) {
    u64 flags = PT_PRESENT;

    if (elf_flags & PF_W)
        flags |= PT_WRITE;

    return flags;
}

u32 load_elf_sections_32(void* elf_file) {
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
        map_region_32(size, vbase, pbase, flags, true);

        // Copy all loadable data from the file
        memcpy((void*)(uintptr_t)pbase, elf_file + p_header->offset, p_header->file_size);

        // Zero out any additional space
        size_t zero_len = p_header->mem_size - p_header->file_size;
        memset((void*)(uintptr_t)pbase + p_header->file_size, 0, zero_len);
    }

    return header->entry;
}
