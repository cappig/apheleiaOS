#include "panic.h"

#include <base/attributes.h>
#include <base/types.h>
#include <log/log.h>

#include "arch/stacktrace.h"


NORETURN void panic_unwind() {
    disble_interrupts();

    dump_stack_trace();
    // TODO: save and dump state

    log_fatal("Kernel panic: halting execution");

    halt();
    __builtin_unreachable();
}
