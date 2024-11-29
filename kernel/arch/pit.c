#include "arch/pit.h"

#include <base/attributes.h>
#include <log/log.h>
#include <x86/asm.h>

#include "arch/idt.h"
#include "arch/irq.h"

static void irq_sys_timer(UNUSED int_state* s) {
    log_warn(".PIT CLOCK.");
}

static void set_timer_freq(usize hz) {
    u16 divisor = (u16)(PIT_BASE_FREQ / hz);

    outb(PIT_CONTROL, PIT_SET);
    outb(PIT_A, divisor & PIT_MASK);
    outb(PIT_A, (divisor >> 8) & PIT_MASK);
}


void pit_init() {
    set_timer_freq(PIT_FREQ);
    irq_register(IRQ_SYSTEM_TIMER, irq_sys_timer);
}
