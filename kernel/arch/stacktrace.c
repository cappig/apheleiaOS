#include "stacktrace.h"

#include <base/addr.h>
#include <base/types.h>
#include <log/log.h>
#include <parse/sym.h>

#include "sys/symbols.h"


void dump_stack_trace(u64 rbp) {
    stack_frame* frame = (stack_frame*)rbp;

    log_debug("Stack trace:");

    for (usize i = 0; frame && i < STACKTRACE_MAX; i++) {
        symbol_entry* sym = resolve_symbol(frame->rip);

        if (!sym) {
            log_debug("<%#lx> (unknown symbol)", frame->rip);
        } else {
            usize offset = frame->rip - sym->addr;
            char* sym_name = sym->name;

            log_debug("<%#lx> %s+%#lx", frame->rip, sym_name, offset);
        }

        frame = (stack_frame*)frame->rbp;
    }
}
