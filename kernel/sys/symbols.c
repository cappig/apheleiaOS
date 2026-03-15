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

static const char *const kernel_elf_paths[] = {
    "/boot/kernel64.elf",
    "/boot/kernel32.elf"
};

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

static bool _name_is_terminated(const char *name, size_t max_len) {
    return name && memchr(name, '\0', max_len);
}

static bool _symbol_section_to_table(
    const elf_view_t *view,
    const elf_section_view_t *sym_sec
) {
    if (!view || !sym_sec) {
        return false;
    }

    if (sym_sec->link >= view->sh_num) {
        return false;
    }

    if (!elf_view_section_data_ok(view, sym_sec)) {
        return false;
    }

    size_t min_ent_size = elf_view_min_symbol_size(view);
    if (!min_ent_size) {
        return false;
    }

    size_t ent_size = sym_sec->ent_size ? sym_sec->ent_size : min_ent_size;
    if (ent_size < min_ent_size || sym_sec->size < ent_size) {
        return false;
    }

    elf_section_view_t str_sec = {0};
    if (
        !elf_view_read_section(view, sym_sec->link, &str_sec) ||
        str_sec.type != SHT_STRTAB ||
        !elf_view_section_data_ok(view, &str_sec)
    ) {
        return false;
    }

    const char *strtab = (const char *)view->blob + str_sec.offset;
    size_t strtab_size = str_sec.size;
    size_t sym_count = sym_sec->size / ent_size;
    size_t text_count = 0;

    for (size_t i = 0; i < sym_count; i++) {
        size_t off = sym_sec->offset + i * ent_size;
        if (!_range_ok(off, ent_size, view->blob_size)) {
            return false;
        }

        elf_symbol_view_t sym = {0};
        if (!elf_view_read_symbol(view, view->blob + off, ent_size, &sym)) {
            return false;
        }

        if (!sym.value || !sym.name) {
            continue;
        }

        if (sym.shndx == 0 || sym.shndx >= view->sh_num) {
            continue;
        }

        elf_section_view_t sec = {0};
        if (!elf_view_read_section(view, sym.shndx, &sec)) {
            return false;
        }

        if (!(sec.flags & SHF_EXECINSTR)) {
            continue;
        }

        if (sym.name >= strtab_size) {
            continue;
        }

        const char *name = strtab + sym.name;
        if (!_name_is_terminated(name, strtab_size - sym.name)) {
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
        size_t off = sym_sec->offset + i * ent_size;

        if (!_range_ok(off, ent_size, view->blob_size)) {
            free(sym_table.map);
            sym_table.map = NULL;
            return false;
        }

        elf_symbol_view_t sym = {0};
        if (!elf_view_read_symbol(view, view->blob + off, ent_size, &sym)) {
            free(sym_table.map);
            sym_table.map = NULL;
            return false;
        }

        if (!sym.value || !sym.name) {
            continue;
        }

        if (sym.shndx == 0 || sym.shndx >= view->sh_num) {
            continue;
        }

        elf_section_view_t sec = {0};
        if (!elf_view_read_section(view, sym.shndx, &sec)) {
            free(sym_table.map);
            sym_table.map = NULL;
            return false;
        }

        if (!(sec.flags & SHF_EXECINSTR)) {
            continue;
        }

        if (sym.name >= strtab_size) {
            continue;
        }

        char *name = sym_blob + str_sec.offset + sym.name;
        if (!_name_is_terminated(name, strtab_size - sym.name)) {
            continue;
        }

        sym_table.map[sym_table.len].addr = sym.value;
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

    size_t total_read = 0;
    while (total_read < blob_size) {
        ssize_t read = vfs_read(
            file, buffer + total_read, total_read, blob_size - total_read, 0
        );

        if (read <= 0) {
            break;
        }
        total_read += (size_t)read;
    }

    if (!total_read) {
        log_warn("failed to read %s", path);
        free(buffer);
        return;
    }

    blob_size = total_read;
    sym_blob = buffer;

    elf_view_t view = {0};
    if (!elf_view_init(&view, sym_blob, blob_size)) {
        log_warn("%s is not a valid ELF with section headers", path);
        _clear_symbols();
        return;
    }

    elf_section_view_t sym_sec = {0};
    bool has_symtab = elf_view_find_section(&view, ".symtab", &sym_sec);
    if (!has_symtab) {
        has_symtab = elf_view_find_section(&view, ".dynsym", &sym_sec);
    }

    if (!has_symtab || (sym_sec.type != SHT_SYMTAB && sym_sec.type != SHT_DYNSYM)) {
        log_warn("no symbol table found in %s", path);
        _clear_symbols();
        return;
    }

    if (!_symbol_section_to_table(&view, &sym_sec)) {
        log_warn("failed to load symbols from %s", path);
        _clear_symbols();
        return;
    }

    log_debug("loaded %zu symbols from %s", sym_table.len, path);
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
