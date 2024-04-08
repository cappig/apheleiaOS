#include "load_elf.h"

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <parse/elf.h>
#include <string.h>
#include <x86/paging.h>

#include "disk.h"
#include "memory.h"
#include "paging.h"
#include "tty.h"


u64 load_elf_sections(file_handle* elf_file) {
    elf_header* header = (elf_header*)elf_file->addr;

    elf_attributes attribs = {0};
    elf_parse_header(&attribs, elf_file->addr);

    u64 size = attribs.top - attribs.base;

    void* kernel_pbase = mmap_alloc(size, E820_KERNEL, attribs.alignment);

    for (usize i = 0; i < header->ph_num; i++) {
        const elf_prog_header* p_header = elf_file->addr + header->phoff + i * header->phent_size;

        if (p_header->type != PT_LOAD)
            continue;

        if (!p_header->file_size && !p_header->mem_size)
            continue;

        u64 segment_pbase = (u64)(uptr)kernel_pbase + (p_header->vaddr - attribs.base);
        u64 segment_vbase = ALIGN_DOWN(p_header->vaddr, PAGE_4KIB);

        u32 flags = elf_to_page_flags(p_header->flags);

        memcpy(
            (void*)(uptr)segment_pbase,
            (void*)(uptr)(elf_file->addr + p_header->offset),
            p_header->file_size
        );

        usize segment_pages = ALIGN(p_header->file_size, PAGE_4KIB) / PAGE_4KIB;

        for (usize j = 0; j < segment_pages; j++) {
            u64 vaddr = segment_vbase + j * PAGE_4KIB;
            u64 paddr = segment_pbase + j * PAGE_4KIB;

            map_page(PAGE_4KIB, vaddr, paddr, flags, true);
        }

        // The segment is larger in memory than on disk i.e. a BSS section
        if (p_header->mem_size > p_header->file_size) {
            u64 bss_vbase = ALIGN_DOWN(p_header->vaddr + p_header->file_size, PAGE_4KIB);
            u64 bss_vtop = p_header->vaddr + p_header->mem_size;

            // TODO: handle this edge case
            // The BSS segment shares a page with another segment
            if (!IS_PAGE_ALIGNED(bss_vbase))
                panic("TODO! Base of BSS segment is not page aligned!");

            u64 bss_memory_size = ALIGN(bss_vtop - bss_vbase, PAGE_4KIB);
            usize bss_pages = bss_memory_size / PAGE_4KIB;

            u64 bss_pbase = (u64)(uptr)mmap_alloc(bss_pages, E820_KERNEL, attribs.alignment);

            memset((void*)(uptr)bss_pbase, 0, bss_pages);

            for (usize j = 0; j < bss_pages; j++) {
                u64 vaddr = bss_vbase + j * PAGE_4KIB;
                u64 paddr = bss_pbase + j * PAGE_4KIB;

                map_page(PAGE_4KIB, vaddr, paddr, flags, true);
            }
        }
    }

    return header->entry;
}
