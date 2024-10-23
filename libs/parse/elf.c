#include "elf.h"

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <x86/paging.h>


bool elf_is_executable(elf_header* header) {
    if (header->type != ET_EXEC)
        return false;

    return true;
}

elf_validity elf_verify(elf_header* header) {
    if (header->magic != ELF_MAGIC)
        return INVALID_ELF;

    if (header->version != ELF_VERSION)
        return INVALID_ELF;

    if (header->arch != EARCH_X64)
        return INVALID_ELF64;

    if (header->endianness != EEND_LITTLE)
        return WRONG_ENDIAN_ELF;

    return VALID_ELF;
}


// Convert elf segment flags to page flags
u64 elf_to_page_flags(u32 elf_flags) {
    u64 flags = PT_PRESENT;

    if (!(elf_flags & PF_X))
        flags |= PT_NO_EXECUTE;

    if (elf_flags & PF_W)
        flags |= PT_WRITE;

    return flags;
}

bool elf_parse_header(elf_attributes* attribs, elf_header* header) {
    u64 ph_base = (u64)(uptr)header + header->phoff;

    attribs->base = (u64)-1;
    attribs->top = 0;
    attribs->alignment = 1;

    bool has_load = false;
    for (usize i = 0; i < header->ph_num; i++) {
        elf_prog_header* p_header = (elf_prog_header*)(uptr)(ph_base + i * header->phent_size);

        if (p_header->type != PT_LOAD)
            continue;

        if (!p_header->file_size && !p_header->mem_size)
            continue;

        has_load = true;

        if (attribs->base > p_header->vaddr)
            attribs->base = p_header->vaddr;

        if (attribs->top < p_header->vaddr + p_header->mem_size)
            attribs->top = p_header->vaddr + p_header->mem_size;

        if (attribs->alignment < p_header->align)
            attribs->alignment = p_header->align;
    }

    return has_load;
}
