#include "panic.h"

#include <base/attributes.h>
#include <base/types.h>
#include <log/log.h>


NORETURN
void panic_unwind() {
    // TODO: dump the stack trace and registers
    // dump_stack_trace(rbp);

    halt();
    __builtin_unreachable();
}
