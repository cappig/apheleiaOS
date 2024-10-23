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


u64 load_elf_sections(file_handle* elf_file) {
    elf_header* header = elf_file->addr;

    for (usize i = 0; i < header->ph_num; i++) {
        elf_prog_header* p_header = elf_file->addr + header->phoff + i * header->phent_size;

        if (p_header->type != PT_LOAD)
            continue;

        if (!p_header->file_size && !p_header->mem_size)
            continue;

        u64 size = ALIGN(p_header->mem_size, PAGE_4KIB);

        u64 flags = elf_to_page_flags(p_header->flags);

        u64 pbase = (u64)(uptr)mmap_alloc(size, E820_KERNEL, p_header->align);
        u64 vbase = p_header->vaddr;

        // Map the segment
        map_region(size, vbase, pbase, flags, true);

        // Copy all loadable data from the file
        memcpy((void*)(uptr)pbase, elf_file->addr + p_header->offset, p_header->file_size);

        // Zero out any additional space
        usize zero_len = p_header->mem_size - p_header->file_size;
        memset((void*)(uptr)pbase + p_header->file_size, 0, zero_len);
    }

    return header->entry;
}
