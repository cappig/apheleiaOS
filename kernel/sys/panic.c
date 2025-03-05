#include "panic.h"

#include <base/attributes.h>
#include <base/types.h>
#include <log/log.h>

#include "arch/irq.h"
#include "sys/tty.h"


void panic_prepare() {
    timer_disable();
    disable_interrupts();

    // Switch to the debug console so that we can dump the error to the screen
    if (current_tty != TTY_NONE)
        tty_set_current(TTY_CONSOLE);
}
