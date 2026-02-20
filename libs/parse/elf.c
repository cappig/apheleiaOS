#include "elf.h"

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <string.h>

enum {
    EI_MAG0 = 0,
    EI_MAG1 = 1,
    EI_MAG2 = 2,
    EI_MAG3 = 3,
    EI_CLASS = 4,
    EI_DATA = 5,
    EI_VERSION = 6,
};

enum {
    ELFCLASS32 = 1,
    ELFCLASS64 = 2,
    ELFDATA2LSB = 1,
};

typedef struct PACKED {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u32 entry;
    u32 phoff;
    u32 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf32_ehdr_t;

typedef struct PACKED {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} elf64_ehdr_t;

typedef struct PACKED {
    u32 name;
    u32 type;
    u32 flags;
    u32 addr;
    u32 offset;
    u32 size;
    u32 link;
    u32 info;
    u32 addralign;
    u32 entsize;
} elf32_shdr_t;

typedef struct PACKED {
    u32 name;
    u32 type;
    u64 flags;
    u64 addr;
    u64 offset;
    u64 size;
    u32 link;
    u32 info;
    u64 addralign;
    u64 entsize;
} elf64_shdr_t;

typedef struct PACKED {
    u32 name;
    u32 value;
    u32 size;
    u8 info;
    u8 other;
    u16 shndx;
} elf32_sym_t;

typedef struct PACKED {
    u32 name;
    u8 info;
    u8 other;
    u16 shndx;
    u64 value;
    u64 size;
} elf64_sym_t;

static bool _range_ok(size_t offset, size_t len, size_t total) {
    if (offset > total) {
        return false;
    }

    if (len > total - offset) {
        return false;
    }

    return true;
}

static bool _elf_ident_ok(const u8 ident[16]) {
    if (!ident) {
        return false;
    }

    if (ident[EI_MAG0] != 0x7f || ident[EI_MAG1] != 'E' || ident[EI_MAG2] != 'L' ||
        ident[EI_MAG3] != 'F') {
        return false;
    }

    if (ident[EI_DATA] != ELFDATA2LSB || ident[EI_VERSION] != 1) {
        return false;
    }

    return ident[EI_CLASS] == ELFCLASS32 || ident[EI_CLASS] == ELFCLASS64;
}

static bool _name_is_terminated(const char *name, size_t max_len) {
    return name && memchr(name, '\0', max_len);
}

static bool _find_section_index(
    const elf_view_t *view,
    const char *name,
    size_t *out_idx,
    elf_section_view_t *out_section
);


bool elf_is_executable(elf_header_t *header) {
    if (header->type != ET_EXEC) {
        return false;
    }

    return true;
}

elf_validity_t elf_verify(elf_header_t *header) {
    if (header->magic != ELF_MAGIC) {
        return INVALID_ELF;
    }

    if (header->version != ELF_VERSION) {
        return INVALID_ELF;
    }

    // if (header->arch != EARCH_X64)
    //     return INVALID_ELF64;

    if (header->endianness != EEND_LITTLE) {
        return WRONG_ENDIAN_ELF;
    }

    return VALID_ELF;
}


u64 elf_to_mmap_prot(u32 elf_flags) {
    u64 prot = 0;

    if (elf_flags & PF_R) {
        prot |= 1 << 0;
    }

    if (elf_flags & PF_W) {
        prot |= 1 << 1;
    }

    if (elf_flags & PF_X) {
        prot |= 1 << 2;
    }

    return prot;
}

bool elf_parse_header(elf_attributes_t *attribs, elf_header_t *header) {
    uintptr_t ph_base = (uintptr_t)header + (uintptr_t)header->phoff;

    attribs->base = (u64)-1;
    attribs->top = 0;
    attribs->alignment = 1;

    bool has_load = false;
    for (size_t i = 0; i < header->ph_num; i++) {
        elf_prog_header_t *p_header =
            (elf_prog_header_t *)(ph_base + (uintptr_t)i * header->phent_size);

        if (p_header->type != PT_LOAD) {
            continue;
        }

        if (!p_header->file_size && !p_header->mem_size) {
            continue;
        }

        has_load = true;

        if (attribs->base > p_header->vaddr) {
            attribs->base = p_header->vaddr;
        }

        if (attribs->top < p_header->vaddr + p_header->mem_size) {
            attribs->top = p_header->vaddr + p_header->mem_size;
        }

        if (attribs->alignment < p_header->align) {
            attribs->alignment = p_header->align;
        }
    }

    return has_load;
}

elf_sect_header_t *elf_locate_section(elf_header_t *header, const char *name) {
    if (!header || !name) {
        return NULL;
    }

    elf_view_t view = {0};
    if (!elf_view_init(&view, header, (size_t)-1)) {
        return NULL;
    }

    size_t idx = 0;
    elf_section_view_t section = {0};
    if (!_find_section_index(&view, name, &idx, &section)) {
        return NULL;
    }

    if (view.elf_class == ELFCLASS64 && view.shent_size >= sizeof(elf_sect_header_t)) {
        return (elf_sect_header_t *)((u8 *)header + view.shoff + idx * view.shent_size);
    }

    static elf_sect_header_t compat_section;
    compat_section.name = section.name;
    compat_section.type = section.type;
    compat_section.flags = section.flags;
    compat_section.addr = section.addr;
    compat_section.offset = section.offset;
    compat_section.size = section.size;
    compat_section.link = section.link;
    compat_section.info = section.info;
    compat_section.align = section.align;
    compat_section.ent_size = section.ent_size;

    return &compat_section;
}

elf_symbol_t *
elf_locate_symbol(elf_symbol_t *symtab, size_t symtab_size, char *strtab, const char *name) {
    if (!symtab || !strtab || !name) {
        return NULL;
    }

    elf_view_t view = {.elf_class = ELFCLASS64};
    size_t ent_size = sizeof(elf_symbol_t);
    size_t count = symtab_size / ent_size;
    for (size_t i = 0; i < count; i++) {
        const u8 *entry = (const u8 *)symtab + i * ent_size;
        elf_symbol_view_t symbol = {0};
        if (!elf_view_read_symbol(&view, entry, ent_size, &symbol)) {
            return NULL;
        }

        char *sym_name = &strtab[symbol.name];

        if (!strcmp(sym_name, name)) {
            return (elf_symbol_t *)entry;
        }
    }

    return NULL;
}

bool elf_view_init(elf_view_t *view, const void *blob, size_t blob_size) {
    if (!view || !blob || blob_size < sizeof(elf32_ehdr_t)) {
        return false;
    }

    const u8 *bytes = blob;
    if (!_elf_ident_ok(bytes)) {
        return false;
    }

    size_t shoff = 0;
    size_t shent_size = 0;
    size_t sh_num = 0;
    size_t shstrndx = 0;
    size_t min_shdr_size = 0;

    if (bytes[EI_CLASS] == ELFCLASS32) {
        const elf32_ehdr_t *hdr = blob;
        shoff = (size_t)hdr->shoff;
        shent_size = (size_t)hdr->shentsize;
        sh_num = (size_t)hdr->shnum;
        shstrndx = (size_t)hdr->shstrndx;
        min_shdr_size = sizeof(elf32_shdr_t);
    } else {
        if (blob_size < sizeof(elf64_ehdr_t)) {
            return false;
        }

        const elf64_ehdr_t *hdr = blob;
        if (hdr->shoff > (u64)(size_t)-1) {
            return false;
        }

        shoff = (size_t)hdr->shoff;
        shent_size = (size_t)hdr->shentsize;
        sh_num = (size_t)hdr->shnum;
        shstrndx = (size_t)hdr->shstrndx;
        min_shdr_size = sizeof(elf64_shdr_t);
    }

    if (!sh_num || !shent_size || shent_size < min_shdr_size) {
        return false;
    }

    if (sh_num > (size_t)-1 / shent_size) {
        return false;
    }

    if (!_range_ok(shoff, sh_num * shent_size, blob_size)) {
        return false;
    }

    if (shstrndx >= sh_num) {
        return false;
    }

    view->blob = bytes;
    view->blob_size = blob_size;
    view->elf_class = bytes[EI_CLASS];
    view->shoff = shoff;
    view->shent_size = shent_size;
    view->sh_num = sh_num;
    view->shstrndx = shstrndx;

    return true;
}

bool elf_view_read_section(const elf_view_t *view, size_t idx, elf_section_view_t *out) {
    if (!view || !out || idx >= view->sh_num) {
        return false;
    }

    size_t shoff = view->shoff + idx * view->shent_size;
    if (!_range_ok(shoff, view->shent_size, view->blob_size)) {
        return false;
    }

    if (view->elf_class == ELFCLASS32) {
        const elf32_shdr_t *raw = (const elf32_shdr_t *)(view->blob + shoff);
        out->name = raw->name;
        out->type = raw->type;
        out->flags = raw->flags;
        out->addr = raw->addr;
        out->offset = (size_t)raw->offset;
        out->size = (size_t)raw->size;
        out->link = raw->link;
        out->info = raw->info;
        out->align = raw->addralign;
        out->ent_size = (size_t)raw->entsize;
        return true;
    }

    const elf64_shdr_t *raw = (const elf64_shdr_t *)(view->blob + shoff);
    if (raw->offset > (u64)(size_t)-1 || raw->size > (u64)(size_t)-1 ||
        raw->entsize > (u64)(size_t)-1) {
        return false;
    }

    out->name = raw->name;
    out->type = raw->type;
    out->flags = raw->flags;
    out->addr = raw->addr;
    out->offset = (size_t)raw->offset;
    out->size = (size_t)raw->size;
    out->link = raw->link;
    out->info = raw->info;
    out->align = raw->addralign;
    out->ent_size = (size_t)raw->entsize;

    return true;
}

bool elf_view_section_data_ok(const elf_view_t *view, const elf_section_view_t *section) {
    if (!view || !section) {
        return false;
    }

    if (section->type == SHT_NOBITS) {
        return true;
    }

    return _range_ok(section->offset, section->size, view->blob_size);
}

static bool _find_section_index(
    const elf_view_t *view,
    const char *name,
    size_t *out_idx,
    elf_section_view_t *out_section
) {
    if (!view || !name || !out_section) {
        return false;
    }

    elf_section_view_t shstr = {0};
    if (!elf_view_read_section(view, view->shstrndx, &shstr) ||
        !elf_view_section_data_ok(view, &shstr)) {
        return false;
    }

    const char *shstr_base = (const char *)view->blob + shstr.offset;
    size_t shstr_size = shstr.size;

    for (size_t i = 0; i < view->sh_num; i++) {
        elf_section_view_t section = {0};
        if (!elf_view_read_section(view, i, &section)) {
            return false;
        }

        if (section.type == SHT_NULL) {
            continue;
        }

        if (section.name >= shstr_size) {
            continue;
        }

        const char *section_name = shstr_base + section.name;
        if (!_name_is_terminated(section_name, shstr_size - section.name)) {
            continue;
        }

        if (!strcmp(section_name, name)) {
            if (out_idx) {
                *out_idx = i;
            }
            *out_section = section;
            return true;
        }
    }

    return false;
}

bool elf_view_find_section(
    const elf_view_t *view,
    const char *name,
    elf_section_view_t *out_section
) {
    return _find_section_index(view, name, NULL, out_section);
}

bool elf_view_read_symbol(
    const elf_view_t *view,
    const u8 *entry,
    size_t ent_size,
    elf_symbol_view_t *out
) {
    if (!view || !entry || !out) {
        return false;
    }

    if (view->elf_class == ELFCLASS32) {
        if (ent_size < sizeof(elf32_sym_t)) {
            return false;
        }

        const elf32_sym_t *raw = (const elf32_sym_t *)entry;
        out->name = raw->name;
        out->shndx = raw->shndx;
        out->value = raw->value;
        return true;
    }

    if (ent_size < sizeof(elf64_sym_t)) {
        return false;
    }

    const elf64_sym_t *raw = (const elf64_sym_t *)entry;
    out->name = raw->name;
    out->shndx = raw->shndx;
    out->value = raw->value;

    return true;
}

size_t elf_view_min_symbol_size(const elf_view_t *view) {
    if (!view) {
        return 0;
    }

    if (view->elf_class == ELFCLASS32) {
        return sizeof(elf32_sym_t);
    }

    if (view->elf_class == ELFCLASS64) {
        return sizeof(elf64_sym_t);
    }

    return 0;
}
