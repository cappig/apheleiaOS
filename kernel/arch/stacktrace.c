#include "stacktrace.h"

#include <base/addr.h>
#include <base/types.h>
#include <log/log.h>
#include <parse/sym.h>

#include "sys/symbols.h"


void dump_stack_trace() {
    u64 rbp = 0;
    asm("movq %%rbp, %0" : "=r"(rbp)::);

    stack_frame* frame = (stack_frame*)rbp;

    log_error("Stack trace:");

    while (frame) {
        isize index = resolve_symbol(frame->rip);

        if (index < 0) {
            log_error("<%#lx> (unknown symbol)", frame->rip);
        } else {
            symbol_entry* sym = get_symbol(index);

            usize offset = frame->rip - sym->addr;
            char* sym_name = sym->name;

            log_error("<%#lx> %s+%#lx", frame->rip, sym_name, offset);
        }

        frame = (stack_frame*)frame->rbp;
    }
}
