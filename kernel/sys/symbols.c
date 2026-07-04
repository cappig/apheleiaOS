#include "symbols.h"

#include <base/macros.h>
#include <errno.h>
#include <log/log.h>
#include <parse/elf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/vfs.h>

typedef struct {
    symbol_table_t table;
    char *blob;
} symbols_state_t;

static symbols_state_t symbols = { 0 };

static const char *const kernel_elf_paths[] = { "/boot/kernel64.elf", "/boot/kernel32.elf" };

static void _clear_symbols(void) {
    if (symbols.table.map) {
        free(symbols.table.map);
        symbols.table.map = NULL;
        symbols.table.len = 0;
    }

    if (symbols.blob) {
        free(symbols.blob);
        symbols.blob = NULL;
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

static bool traceable_name(const char *name) {
    if (!name || !*name) {
        return false;
    }

    return name[0] != '$' && strncmp(name, ".L", 2) != 0;
}

static bool traceable_symbol(const elf_symbol_view_t *sym, const char *name) {
    if (!sym) {
        return false;
    }

    u8 type = ELF_SYMBOL_TYPE(sym->info);

    if (type == STT_FUNC) {
        return true;
    }

    if (type == STT_NOTYPE) {
        return traceable_name(name);
    }

    return false;
}

typedef struct {
    const elf_view_t *view;
    const elf_section_view_t *section;
    const char *names;
    size_t name_size;
    size_t ent_size;
    size_t count;
} symbol_scan_t;

static bool symbol_scan_init(const elf_view_t *view, const elf_section_view_t *sym_sec, symbol_scan_t *scan) {
    if (!view || !sym_sec) {
        return false;
    }

    if (sym_sec->link >= view->sh_num) {
        return false;
    }

    if (!elf_section_data_ok(view, sym_sec)) {
        return false;
    }

    size_t min_ent_size = elf_min_symbol_size(view);
    if (!min_ent_size) {
        return false;
    }

    size_t ent_size = sym_sec->ent_size ? sym_sec->ent_size : min_ent_size;
    if (ent_size < min_ent_size || sym_sec->size < ent_size) {
        return false;
    }

    elf_section_view_t str_sec = { 0 };
    if (!elf_view_read_section(view, sym_sec->link, &str_sec) || str_sec.type != SHT_STRTAB ||
        !elf_section_data_ok(view, &str_sec)) {
        return false;
    }

    *scan = (symbol_scan_t){
        .view = view,
        .section = sym_sec,
        .names = (const char *)view->blob + str_sec.offset,
        .name_size = str_sec.size,
        .ent_size = ent_size,
        .count = sym_sec->size / ent_size,
    };

    return true;
}

static int
symbol_scan_read(const symbol_scan_t *scan, size_t index, elf_symbol_view_t *sym_out, const char **name_out) {
    size_t off = scan->section->offset + index * scan->ent_size;
    if (!_range_ok(off, scan->ent_size, scan->view->blob_size)) {
        return -EINVAL;
    }

    elf_symbol_view_t sym = { 0 };
    if (!elf_view_read_symbol(scan->view, scan->view->blob + off, scan->ent_size, &sym)) {
        return -EINVAL;
    }

    if (!sym.value || !sym.name) {
        return 0;
    }

    if (sym.shndx == 0 || sym.shndx >= scan->view->sh_num) {
        return 0;
    }

    elf_section_view_t sec = { 0 };
    if (!elf_view_read_section(scan->view, sym.shndx, &sec)) {
        return -EINVAL;
    }

    if (!(sec.flags & SHF_EXECINSTR)) {
        return 0;
    }

    if (sym.name >= scan->name_size) {
        return 0;
    }

    const char *name = scan->names + sym.name;
    if (!_name_is_terminated(name, scan->name_size - sym.name)) {
        return 0;
    }

    if (!traceable_symbol(&sym, name)) {
        return 0;
    }

    *sym_out = sym;
    *name_out = name;
    return 1;
}

static void symbol_table_free(void) {
    free(symbols.table.map);
    symbols.table.map = NULL;
    symbols.table.len = 0;
}

static bool symbol_section_ok(const elf_view_t *view, const elf_section_view_t *sym_sec) {
    symbol_scan_t scan = { 0 };
    if (!symbol_scan_init(view, sym_sec, &scan)) {
        return false;
    }

    size_t text_count = 0;
    for (size_t i = 0; i < scan.count; i++) {
        elf_symbol_view_t sym = { 0 };
        const char *name = NULL;

        int status = symbol_scan_read(&scan, i, &sym, &name);
        if (status < 0) {
            return false;
        }
        if (status > 0) {
            text_count++;
        }
    }

    if (!text_count || text_count > (size_t)-1 / sizeof(symbol_entry_t)) {
        return false;
    }

    symbols.table.map = malloc(text_count * sizeof(symbol_entry_t));
    if (!symbols.table.map) {
        return false;
    }

    symbols.table.len = 0;

    for (size_t i = 0; i < scan.count && symbols.table.len < text_count; i++) {
        elf_symbol_view_t sym = { 0 };
        const char *name = NULL;

        int status = symbol_scan_read(&scan, i, &sym, &name);
        if (status < 0) {
            symbol_table_free();
            return false;
        }
        if (!status) {
            continue;
        }

        symbols.table.map[symbols.table.len].addr = sym.value;
        symbols.table.map[symbols.table.len].name = (char *)name;
        symbols.table.len++;
    }

    if (!symbols.table.len) {
        symbol_table_free();
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
        ssize_t read = vfs_read(file, buffer + total_read, total_read, blob_size - total_read, 0);

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
    symbols.blob = buffer;

    elf_view_t view = { 0 };
    if (!elf_view_init(&view, symbols.blob, blob_size)) {
        log_warn("%s is not a valid ELF with section headers", path);
        _clear_symbols();
        return;
    }

    elf_section_view_t sym_sec = { 0 };
    bool has_symtab = elf_view_find_section(&view, ".symtab", &sym_sec);
    if (!has_symtab) {
        has_symtab = elf_view_find_section(&view, ".dynsym", &sym_sec);
    }

    if (!has_symtab || (sym_sec.type != SHT_SYMTAB && sym_sec.type != SHT_DYNSYM)) {
        log_warn("no symbol table found in %s", path);
        _clear_symbols();
        return;
    }

    if (!symbol_section_ok(&view, &sym_sec)) {
        log_warn("failed to load symbols from %s", path);
        _clear_symbols();
        return;
    }

    log_debug("loaded %zu symbols from %s", symbols.table.len, path);
}

symbol_entry_t *resolve_symbol(u64 addr) {
    if (!symbols.table.len || !symbols.table.map) {
        return NULL;
    }

    ssize_t index = -1;
    u64 best_addr = 0;

    for (size_t i = 0; i < symbols.table.len; i++) {
        symbol_entry_t *sym = &symbols.table.map[i];
        u64 cur_addr = sym->addr;

        if (cur_addr <= addr && cur_addr >= best_addr) {
            index = (ssize_t)i;
            best_addr = cur_addr;
        }
    }

    if (index < 0) {
        return NULL;
    }

    return &symbols.table.map[index];
}

const char *resolve_symbol_name(u64 addr) {
    symbol_entry_t *sym = resolve_symbol(addr);
    if (sym) {
        return sym->name;
    }

    return "(unknown symbol)";
}
