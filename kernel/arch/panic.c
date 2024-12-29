#include "panic.h"

#include <base/attributes.h>
#include <base/types.h>
#include <log/log.h>

#include "arch/irq.h"
#include "arch/stacktrace.h"


NORETURN void panic_unwind() {
    timer_disable();
    disable_interrupts();

    // TODO: save and dump state
    dump_stack_trace();

    log_fatal("Kernel panic - halting execution!");

    // FIXME: what if we panic before the ttys get initialized

    halt();
    __builtin_unreachable();
}
