#include "symbols.h"

#include <base/macros.h>
#include <log/log.h>
#include <parse/elf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/vfs.h>

static symbol_table_t sym_table = {0};
static char *sym_blob = NULL;

static const char *const kernel_elf_paths[] = {"/boot/kernel64.elf", "/boot/kernel32.elf"};

static void _clear_symbols(void) {
    if (sym_table.map) {
        free(sym_table.map);
        sym_table.map = NULL;
        sym_table.len = 0;
    }

    if (sym_blob) {
        free(sym_blob);
        sym_blob = NULL;
    }
}

static bool _range_ok(size_t offset, size_t len, size_t total) {
    if (offset > total) {
        return false;
    }

    if (len > total - offset) {
        return false;
    }

    return true;
}

static bool _elf_layout_ok(elf_header_t *elf, size_t blob_size) {
    if (!elf || blob_size < sizeof(elf_header_t)) {
        return false;
    }

    if (elf_verify(elf) != VALID_ELF) {
        return false;
    }

    if (!elf->sh_num || !elf->shdr_size) {
        return false;
    }

    size_t shoff = (size_t)elf->shoff;
    size_t shdr_size = (size_t)elf->shdr_size;
    size_t sh_num = (size_t)elf->sh_num;

    if (sh_num > (size_t)-1 / shdr_size) {
        return false;
    }

    if (!_range_ok(shoff, sh_num * shdr_size, blob_size)) {
        return false;
    }

    if (elf->shstrndx >= elf->sh_num) {
        return false;
    }

    return true;
}

static elf_sect_header_t *_section_at(elf_header_t *elf, size_t idx) {
    return (elf_sect_header_t *)((u8 *)elf + (size_t)elf->shoff + idx * (size_t)elf->shdr_size);
}

static bool _name_is_terminated(const char *name, size_t max_len) {
    return name && memchr(name, '\0', max_len);
}

static bool _symbol_section_to_table(elf_header_t *elf, size_t blob_size, elf_sect_header_t *sym_sec) {
    if (!elf || !sym_sec) {
        return false;
    }

    if (sym_sec->link >= elf->sh_num) {
        return false;
    }

    if (!_range_ok((size_t)sym_sec->offset, (size_t)sym_sec->size, blob_size)) {
        return false;
    }

    size_t ent_size = (size_t)(sym_sec->ent_size ? sym_sec->ent_size : sizeof(elf_symbol_t));
    if (ent_size < sizeof(elf_symbol_t)) {
        return false;
    }

    if (sym_sec->size < ent_size) {
        return false;
    }

    elf_sect_header_t *str_sec = _section_at(elf, sym_sec->link);
    if (!_range_ok((size_t)str_sec->offset, (size_t)str_sec->size, blob_size)) {
        return false;
    }

    const char *strtab = (const char *)elf + (size_t)str_sec->offset;
    size_t strtab_size = (size_t)str_sec->size;
    size_t sym_count = (size_t)sym_sec->size / ent_size;
    size_t text_count = 0;

    for (size_t i = 0; i < sym_count; i++) {
        elf_symbol_t *sym = (elf_symbol_t *)((u8 *)elf + (size_t)sym_sec->offset + i * ent_size);

        if (!sym->value || !sym->name) {
            continue;
        }

        if (sym->shndx == 0 || sym->shndx >= elf->sh_num) {
            continue;
        }

        elf_sect_header_t *sec = _section_at(elf, sym->shndx);
        if (!(sec->flags & SHF_EXECINSTR)) {
            continue;
        }

        if (sym->name >= strtab_size) {
            continue;
        }

        const char *name = strtab + sym->name;
        if (!_name_is_terminated(name, strtab_size - sym->name)) {
            continue;
        }

        text_count++;
    }

    if (!text_count) {
        return false;
    }

    sym_table.map = malloc(text_count * sizeof(symbol_entry_t));
    if (!sym_table.map) {
        return false;
    }

    sym_table.len = 0;

    for (size_t i = 0; i < sym_count && sym_table.len < text_count; i++) {
        elf_symbol_t *sym = (elf_symbol_t *)((u8 *)elf + (size_t)sym_sec->offset + i * ent_size);

        if (!sym->value || !sym->name) {
            continue;
        }

        if (sym->shndx == 0 || sym->shndx >= elf->sh_num) {
            continue;
        }

        elf_sect_header_t *sec = _section_at(elf, sym->shndx);
        if (!(sec->flags & SHF_EXECINSTR)) {
            continue;
        }

        if (sym->name >= strtab_size) {
            continue;
        }

        char *name = sym_blob + (size_t)str_sec->offset + sym->name;
        if (!_name_is_terminated(name, strtab_size - sym->name)) {
            continue;
        }

        sym_table.map[sym_table.len].addr = sym->value;
        sym_table.map[sym_table.len].name = name;
        sym_table.len++;
    }

    if (!sym_table.len) {
        free(sym_table.map);
        sym_table.map = NULL;
        return false;
    }

    return true;
}

void load_symbols(void) {
    vfs_node_t *file = NULL;
    const char *path = NULL;

    for (size_t i = 0; i < ARRAY_LEN(kernel_elf_paths); i++) {
        file = vfs_lookup(kernel_elf_paths[i]);
        if (file) {
            path = kernel_elf_paths[i];
            break;
        }
    }

    _clear_symbols();

    if (!file) {
        log_warn("kernel ELF not found in /boot");
        return;
    }

    if (!file->size) {
        log_warn("%s is empty", path);
        return;
    }

    if (file->size > (u64)(size_t)-1) {
        log_warn("%s too large to parse", path);
        return;
    }

    size_t blob_size = (size_t)file->size;
    char *buffer = malloc(blob_size);
    if (!buffer) {
        log_warn("failed to allocate buffer");
        return;
    }

    ssize_t read = vfs_read(file, buffer, 0, blob_size, 0);
    if (read <= 0) {
        log_warn("failed to read %s", path);
        free(buffer);
        return;
    }

    blob_size = (size_t)read;
    sym_blob = buffer;

    elf_header_t *elf = (elf_header_t *)sym_blob;
    if (!_elf_layout_ok(elf, blob_size)) {
        log_warn("%s is not a valid ELF with section headers", path);
        _clear_symbols();
        return;
    }

    elf_sect_header_t *sym_sec = elf_locate_section(elf, ".symtab");
    if (!sym_sec) {
        sym_sec = elf_locate_section(elf, ".dynsym");
    }

    if (!sym_sec) {
        log_warn("no symbol table found in %s", path);
        _clear_symbols();
        return;
    }

    if (!_symbol_section_to_table(elf, blob_size, sym_sec)) {
        log_warn("failed to load symbols from %s", path);
        _clear_symbols();
        return;
    }

    log_debug("loaded %zu entries from %s", sym_table.len, path);
}

symbol_entry_t *resolve_symbol(u64 addr) {
    if (!sym_table.len || !sym_table.map) {
        return NULL;
    }

    ssize_t index = -1;
    u64 best_addr = 0;

    for (size_t i = 0; i < sym_table.len; i++) {
        symbol_entry_t *sym = &sym_table.map[i];

        u64 cur_addr = sym->addr;

        if (cur_addr <= addr && cur_addr >= best_addr) {
            index = (ssize_t)i;
            best_addr = cur_addr;
        }
    }

    if (index < 0) {
        return NULL;
    }

    return &sym_table.map[index];
}

const char *resolve_symbol_name(u64 addr) {
    symbol_entry_t *sym = resolve_symbol(addr);

    if (sym) {
        return sym->name;
    }

    return "(unknown symbol)";
}
