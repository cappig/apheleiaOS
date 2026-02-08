#include "x86/asm.h"

void panic_prepare(void) {
    // timer_disable();
    disable_interrupts();

    // Switch to the debug console so that we can dump the error to the screen
    // if (current_tty != TTY_NONE)
    //     tty_set_current(TTY_CONSOLE);
}

void panic_halt(void) {
    halt();
}
