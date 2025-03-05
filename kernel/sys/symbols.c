#include <base/addr.h>
#include <base/types.h>
#include <boot/proto.h>
#include <log/log.h>
#include <parse/sym.h>
#include <stddef.h>

#include "mem/heap.h"
#include "vfs/fs.h"

static symbol_table sym_table = {0};


void load_symbols() {
    vfs_node* file = vfs_lookup("/boot/sym.map");

    if (!file) {
        log_warn("Kernel symbol table not found!");
        return;
    }

    char* buffer = kmalloc(file->size);

    vfs_read(file, buffer, 0, file->size, 0);

    usize lines = sym_count(buffer, file->size);

    if (!lines) {
        log_warn("Empty kernel symbol file!");

        kfree(buffer);
        return;
    }

    sym_table.map = kmalloc(lines * sizeof(symbol_entry));
    sym_table.len = lines;

    if (!sym_parse(buffer, &sym_table)) {
        log_warn("Error parsing kernel symbols!");

        kfree(sym_table.map);
        sym_table.len = 0;
    }

    kfree(buffer);

    log_debug("Loaded kernel symbols");
}


symbol_entry* resolve_symbol(u64 addr) {
    if (!sym_table.len || !sym_table.map)
        return NULL;

    isize index = -1;
    u64 best_addr = 0;

    // We have to find the biggest address smaller than the addr
    for (usize i = 0; i < sym_table.len; i++) {
        u64 cur_addr = sym_table.map[i].addr;

        if ((cur_addr > best_addr) && (cur_addr < addr)) {
            index = i;
            best_addr = cur_addr;
        }
    }

    if (index == -1)
        return NULL;

    return &sym_table.map[index];
}

char* resolve_symbol_name(u64 addr) {
    symbol_entry* sym = resolve_symbol(addr);

    if (sym)
        return sym->name;

    return "(unknown symbol)";
}
