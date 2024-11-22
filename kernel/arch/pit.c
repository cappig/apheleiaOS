#include "arch/pit.h"

#include <base/attributes.h>
#include <x86/asm.h>

#include "arch/idt.h"
#include "arch/pic.h"
#include "log/log.h"

static void irq_sys_timer(UNUSED int_state* s) {
    log_warn(".clock.");
}

static void set_timer_freq(usize hz) {
    u16 divisor = (u16)(PIT_BASE_FREQ / hz);

    outb(PIT_CONTROL, PIT_SET);
    outb(PIT_A, divisor & PIT_MASK);
    outb(PIT_A, (divisor >> 8) & PIT_MASK);
}


// We use the legacy PIT as a fallback device in case the APIC does't init
// The PIT can only work correctly in a single core setup
void pit_init() {
    set_timer_freq(PIT_FREQ);

    set_int_handler(IRQ_NUMBER(IRQ_SYSTEM_TIMER), irq_sys_timer);
    pic_clear_mask(IRQ_SYSTEM_TIMER);
}
