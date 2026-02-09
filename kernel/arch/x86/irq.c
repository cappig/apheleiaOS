#include "irq.h"

#include <base/attributes.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <x86/asm.h>
#include <x86/pic.h>
#include <x86/pit.h>

static volatile u64 irq_tick_count = 0;

static void _register_legacy(size_t irq, int_handler_t handler) {
#if defined(__i386__)
    if (irq < 8)
        set_int_handler(0x08 + irq, handler);
    else
        set_int_handler(0x70 + (irq - 8), handler);
#else
    (void)irq;
    (void)handler;
#endif
}

static void _timer_handler(int_state_t* state) {
    irq_tick_count++;
    irq_ack(IRQ_SYSTEM_TIMER);
    sched_tick(state);
}

bool irq_init(void) {
    pit_init();
    irq_register(IRQ_SYSTEM_TIMER, _timer_handler);

    log_info("irq: PIT timer initialized at %u Hz", pit_get_frequency());
    return true;
}

void irq_register(size_t irq, int_handler_t handler) {
    size_t vec = IRQ_INT(irq);

    if (!handler || vec >= ISR_COUNT)
        return;

    set_int_handler(vec, handler);
    _register_legacy(irq, handler);

    pic_clear_mask((u8)irq);
}

void irq_ack(size_t irq) {
    pic_end_int(irq);
}

u64 irq_ticks(void) {
    return irq_tick_count;
}

void timer_enable(void) {
    pic_clear_mask(IRQ_SYSTEM_TIMER);
}

void timer_disable(void) {
    pic_set_mask(IRQ_SYSTEM_TIMER);
}
