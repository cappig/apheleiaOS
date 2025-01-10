#include <base/addr.h>
#include <base/types.h>
#include <boot/proto.h>
#include <log/log.h>
#include <parse/sym.h>
#include <stddef.h>

#include "drivers/initrd.h"
#include "mem/heap.h"

static symbol_table sym_table = {0};


void load_symbols() {
    ustar_file* sym_file = initrd_find(INITRD_SYMTAB_NAME);

    if (!sym_file)
        return;

    void* symtab_vaddr = sym_file->data;
    usize symtab_size = ustar_to_num(sym_file->header.size, 12);

    usize lines = sym_count(symtab_vaddr, symtab_size);

    if (!lines)
        return;

    sym_table.map = kmalloc(lines * sizeof(symbol_entry));
    sym_table.len = lines;

    if (!sym_parse(symtab_vaddr, &sym_table)) {
        kfree(sym_table.map);
        sym_table.len = 0;
    }
}


isize resolve_symbol(u64 addr) {
    if (!sym_table.len || !sym_table.map)
        return -1;

    isize ret_index = 0;
    u64 ret_addr = 0;

    // We have to find the biggest address smaller than the addr
    for (usize i = 0; i < sym_table.len; i++) {
        u64 cur_addr = sym_table.map[i].addr;

        if ((cur_addr > ret_addr) && (cur_addr < addr)) {
            ret_index = i;
            ret_addr = cur_addr;
        }
    }

    return ret_index;
}

char* resolve_symbol_name(u64 addr) {
    isize index = resolve_symbol(addr);

    if (index < 0)
        return "(unknown symbol)";
    else
        return sym_table.map[index].name;
}


symbol_entry* get_symbol(usize index) {
    if (!sym_table.len || !sym_table.map)
        return NULL;

    if (index >= sym_table.len)
        return NULL;

    return &sym_table.map[index];
}
