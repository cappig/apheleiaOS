#include "arch/pit.h"

#include <base/attributes.h>
#include <log/log.h>
#include <x86/asm.h>

#include "sys/clock.h"

static void set_timer_freq(usize hz) {
    u16 divisor = (u16)(PIT_BASE_FREQ / hz);

    outb(PIT_CONTROL, PIT_SET);
    outb(PIT_A, divisor & 0xff);
    outb(PIT_A, (divisor >> 8) & 0xff);
}


void pit_init() {
    set_timer_freq(CLOCK_FREQ);
}
