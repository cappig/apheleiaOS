#include <arch/arch.h>
#include "x86/asm.h"

void panic_prepare(void) {
    disable_interrupts();
    arch_panic_enter();
}

void panic_halt(void) {
    halt();
}
