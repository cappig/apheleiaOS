#include "symbols.h"

#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/vfs.h>

static symbol_table_t sym_table = {0};
static char *sym_blob = NULL;

void load_symbols(void) {
    vfs_node_t *file = vfs_lookup("/boot/sym.map");

    if (!file) {
        log_warn("/boot/sym.map not found");
        return;
    }

    if (!file->size) {
        log_warn("/boot/sym.map is empty");
        return;
    }

    if (sym_table.map) {
        free(sym_table.map);
        sym_table.map = NULL;
        sym_table.len = 0;
    }

    if (sym_blob) {
        free(sym_blob);
        sym_blob = NULL;
    }

    char *buffer = malloc(file->size + 1);
    if (!buffer) {
        log_warn("failed to allocate buffer");
        return;
    }

    ssize_t read = vfs_read(file, buffer, 0, file->size, 0);
    if (read <= 0) {
        log_warn("failed to read /boot/sym.map");
        free(buffer);
        return;
    }

    buffer[read] = '\0';
    sym_blob = buffer;

    size_t lines = sym_count(buffer, (size_t)read);
    if (!lines) {
        log_warn("/boot/sym.map has no entries");
        free(sym_blob);
        sym_blob = NULL;
        return;
    }

    sym_table.map = malloc(lines * sizeof(symbol_entry_t));
    if (!sym_table.map) {
        log_warn("failed to allocate symbol table");
        free(buffer);
        return;
    }

    sym_table.len = lines;

    if (!sym_parse(sym_blob, &sym_table)) {
        log_warn("failed to parse /boot/sym.map");
        free(sym_table.map);
        sym_table.map = NULL;
        sym_table.len = 0;
        free(sym_blob);
        sym_blob = NULL;
    }

    log_debug("loaded %zu entries", sym_table.len);
}

static bool _symbol_is_text(const symbol_entry_t *sym) {
    return sym->type == ST_GLOBAL_TEXT || sym->type == ST_LOCAL_TEXT;
}

symbol_entry_t *resolve_symbol(u64 addr) {
    if (!sym_table.len || !sym_table.map) {
        return NULL;
    }

    ssize_t index = -1;
    u64 best_addr = 0;

    for (size_t i = 0; i < sym_table.len; i++) {
        symbol_entry_t *sym = &sym_table.map[i];

        if (!_symbol_is_text(sym)) {
            continue;
        }

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
