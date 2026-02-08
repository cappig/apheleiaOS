#include "paging32.h"

#include <base/attributes.h>
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
    for (u32 i = 0; i < top_address; i += PAGE_4KIB) {
        map_page_32(PAGE_4KIB, i + offset, i, PT_WRITE, is_kernel);
    }
}

void setup_paging_32(void) {
    /* Allocate the page directory (root) */
    pdir = (page_t*)mmap_alloc(PAGE_4KIB, E820_KERNEL, PAGE_4KIB);
    memset(pdir, 0, PAGE_4KIB);

    /* write physical base into CR3 */
    write_cr3((u32)(uintptr_t)pdir);

    /* Note: no NX config here (non-PAE 32-bit typically doesn't support NX).
       If you need NX on 32-bit, you must enable PAE and use appropriate MSRs. */
}

void init_paging_32(void) {
    /* Enable paging by setting CR0.PG */
    u32 cr0 = read_cr0();
    write_cr0(cr0 | CR0_PG);
}


typedef struct PACKED {
    u32 magic;
    u8 arch;
    u8 endianness;
    u8 id_version;
    u8 abi;
    u8 abi_version;
    u8 _unused0[7];

    u16 type;
    u16 machine;
    u32 version;

    u32 entry;

    u32 phoff;
    u32 shoff;

    u32 flags;
    u16 hdr_size;

    u16 phent_size;
    u16 ph_num;

    u16 shdr_size;
    u16 sh_num;

    u16 shstrndx;
} elf32_header_t;

typedef struct PACKED {
    u32 type;
    u32 offset;
    u32 vaddr;
    u32 paddr;
    u32 file_size;
    u32 mem_size;
    u32 flags;
    u32 align;
} elf32_prog_header_t;

static page_t _elf_to_page_flags(u32 elf_flags) {
    u64 flags = PT_PRESENT;

    if (elf_flags & PF_W)
        flags |= PT_WRITE;

    return flags;
}

u32 load_elf_sections_32(void* elf_file) {
    elf32_header_t* header = elf_file;

    for (size_t i = 0; i < header->ph_num; i++) {
        elf32_prog_header_t* p_header =
            (elf32_prog_header_t*)((u8*)elf_file + header->phoff + i * header->phent_size);

        if (p_header->type != PT_LOAD)
            continue;

        if (!p_header->file_size && !p_header->mem_size)
            continue;

        u32 size = ALIGN(p_header->mem_size, PAGE_4KIB);

        u32 flags = _elf_to_page_flags(p_header->flags);

        u32 pbase = (u32)(uintptr_t)mmap_alloc(size, E820_KERNEL, p_header->align);
        u32 vbase = p_header->vaddr;

        // Map the segment
        map_region_32(size, vbase, pbase, flags, true);

        // Copy all loadable data from the file
        memcpy((void*)(uintptr_t)pbase, (u8*)elf_file + p_header->offset, p_header->file_size);

        // Zero out any additional space
        size_t zero_len = p_header->mem_size - p_header->file_size;
        memset((void*)(uintptr_t)pbase + p_header->file_size, 0, zero_len);
    }

    return header->entry;
}
