#include "stacktrace.h"

#include <base/addr.h>
#include <base/types.h>
#include <log/log.h>
#include <parse/sym.h>

#include "mem/heap.h"

static symbol_table sym_table = {0};


void load_symbols(boot_handoff* handoff) {
    if (!handoff->symtab_loc || !handoff->symtab_size)
        return;

    char* symtab_vaddr = (char*)ID_MAPPED_VADDR(handoff->symtab_loc);
    usize symtab_size = handoff->symtab_size;

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
    if (!sym_table.len)
        return 0;

    isize ret_index = -1;
    u64 ret_addr = 0;

    // We have to find the biggest address smaller than the addr
    for (usize i = 0; i < sym_table.len; i++) {
        u64 cur_addr = sym_table.map[i].addr;

        if ((cur_addr >= ret_addr) && (cur_addr <= addr)) {
            ret_index = i;
            ret_addr = cur_addr;
        }
    }

    return ret_index;
}


void dump_stack_trace() {
    u64 rbp = 0;
    asm("movq %%rbp, %0" : "=r"(rbp)::);

    stack_frame* frame = (stack_frame*)rbp;

    log_debug("Dump of stack trace:");

    if (!sym_table.len) {
        log_debug("  No symbol table loaded!");
        return;
    }

    while (frame) {
        isize index = resolve_symbol(frame->rip);

        if (index < 0) {
            log_debug("<%#lx> (unknown symbol)", frame->rip);
        } else {
            u64 sym_addr = sym_table.map[index].addr;
            usize offset = frame->rip - sym_addr;
            char* sym_name = sym_table.map[index].name;

            log_debug("<%#lx> %s+%#lx", frame->rip, sym_name, offset);
        }

        frame = (stack_frame*)frame->rbp;
    }
}
