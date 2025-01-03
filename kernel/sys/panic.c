#include "panic.h"

#include <base/attributes.h>
#include <base/types.h>
#include <log/log.h>

#include "arch/irq.h"
#include "arch/stacktrace.h"
#include "sys/tty.h"


void panic_unwind() {
    timer_disable();
    disable_interrupts();

    // Switch to the debug console so that we can dump the error to the screen
    tty_set_current(TTY_CONSOLE);

    dump_stack_trace();
}
