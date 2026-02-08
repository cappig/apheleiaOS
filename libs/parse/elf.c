#include "elf.h"

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <string.h>


bool elf_is_executable(elf_header_t* header) {
    if (header->type != ET_EXEC)
        return false;

    return true;
}

elf_validity_t elf_verify(elf_header_t* header) {
    if (header->magic != ELF_MAGIC)
        return INVALID_ELF;

    if (header->version != ELF_VERSION)
        return INVALID_ELF;

    // if (header->arch != EARCH_X64)
    //     return INVALID_ELF64;

    if (header->endianness != EEND_LITTLE)
        return WRONG_ENDIAN_ELF;

    return VALID_ELF;
}


u64 elf_to_mmap_prot(u32 elf_flags) {
    u64 prot = 0;

    if (elf_flags & PF_R)
        prot |= 1 << 0;

    if (elf_flags & PF_W)
        prot |= 1 << 1;

    if (elf_flags & PF_X)
        prot |= 1 << 2;

    return prot;
}

bool elf_parse_header(elf_attributes_t* attribs, elf_header_t* header) {
    uintptr_t ph_base = (uintptr_t)header + (uintptr_t)header->phoff;

    attribs->base = (u64)-1;
    attribs->top = 0;
    attribs->alignment = 1;

    bool has_load = false;
    for (size_t i = 0; i < header->ph_num; i++) {
        elf_prog_header_t* p_header =
            (elf_prog_header_t*)(ph_base + (uintptr_t)i * header->phent_size);

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

elf_sect_header_t* elf_locate_section(elf_header_t* header, const char* name) {
    void* soff = (void*)header + header->shoff;
    elf_sect_header_t* shst_sect = soff + header->shstrndx * header->shdr_size;

    char* str_table = (void*)header + shst_sect->offset;

    for (size_t i = 0; i < header->sh_num; i++) {
        elf_sect_header_t* s_header = (void*)header + header->shoff + i * header->shdr_size;

        if (s_header->type == SHT_NULL)
            continue;

        char* sect_name = str_table + s_header->name;

        if (!strcmp(sect_name, name))
            return s_header;
    }

    return NULL;
}

elf_symbol_t*
elf_locate_symbol(elf_symbol_t* symtab, size_t symtab_size, char* strtab, const char* name) {
    size_t count = symtab_size / sizeof(elf_symbol_t);

    for (size_t i = 0; i < count; i++) {
        elf_symbol_t* sym = &symtab[i];
        char* sym_name = &strtab[sym->name];

        if (!strcmp(sym_name, name))
            return sym;
    }

    return NULL;
}
