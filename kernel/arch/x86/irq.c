#include "irq.h"

#include <base/attributes.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <x86/apic.h>
#include <x86/asm.h>
#include <x86/pic.h>
#include <x86/pit.h>

static volatile u64 irq_tick_count = 0;
static bool use_apic_timer = false;
static bool use_ioapic = false;

static void _route_irqs_to_pic(void) {
    outb(0x22, 0x70);
    u8 imcr = inb(0x23);
    outb(0x23, (u8)(imcr & ~0x01));
}

static void _route_irqs_to_apic(void) {
    outb(0x22, 0x70);
    u8 imcr = inb(0x23);
    outb(0x23, (u8)(imcr | 0x01));
}

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

static void _spurious_handler(UNUSED int_state_t* state) {
}

bool irq_init(void) {
    bool apic_ok = apic_init();
    if (apic_ok)
        set_int_handler(INT_SPURIOUS, _spurious_handler);

    if (apic_ok && ioapic_available()) {
        use_ioapic = true;
        ioapic_mask_all();
        pic_mask_all();
        _route_irqs_to_apic();
        log_info("irq: using IOAPIC for external interrupts");
    } else {
        _route_irqs_to_pic();
    }

    if (apic_ok && apic_timer_init(PIT_DEFAULT_HZ))
        use_apic_timer = true;

    if (!use_apic_timer)
        pit_init();

    irq_register(IRQ_SYSTEM_TIMER, timer_handler);

    if (use_apic_timer) {
        if (use_ioapic)
            ioapic_mask_irq(IRQ_SYSTEM_TIMER, true);
        else
            pic_set_mask(IRQ_SYSTEM_TIMER);

        apic_timer_enable();
        log_info("irq: APIC timer initialized at %u Hz", apic_timer_hz());
        return true;
    }

    if (apic_ok)
        log_warn("irq: APIC timer calibration failed, falling back to PIT");

    log_info("irq: PIT timer initialized at %u Hz", pit_get_frequency());
    return true;
}

void irq_register(size_t irq, int_handler_t handler) {
    size_t vec = IRQ_INT(irq);

    if (!handler || vec >= ISR_COUNT)
        return;

    set_int_handler(vec, handler);
    _register_legacy(irq, handler);

    if (use_ioapic) {
        if (irq != IRQ_SYSTEM_TIMER || !use_apic_timer) {
            u32 dest = lapic_id();
            if (!ioapic_route_irq((u8)irq, (u8)vec, dest))
                log_warn("irq: failed to route irq %u via IOAPIC", (u32)irq);
        }
        return;
    }

    if (!use_apic_timer || irq != IRQ_SYSTEM_TIMER)
        pic_clear_mask((u8)irq);
}

void irq_ack(size_t irq) {
    if (use_ioapic) {
        lapic_end_int();
        return;
    }

    if (use_apic_timer && irq == IRQ_SYSTEM_TIMER) {
        lapic_end_int();
        return;
    }

    pic_end_int(irq);
}

bool irq_using_ioapic(void) {
    return use_ioapic;
}

u64 irq_ticks(void) {
    return irq_tick_count;
}

void timer_enable(void) {
    if (use_apic_timer) {
        apic_timer_enable();
        return;
    }

    if (use_ioapic)
        ioapic_mask_irq(IRQ_SYSTEM_TIMER, false);
    else
        pic_clear_mask(IRQ_SYSTEM_TIMER);
}

void timer_disable(void) {
    if (use_apic_timer) {
        apic_timer_disable();
        return;
    }

    if (use_ioapic)
        ioapic_mask_irq(IRQ_SYSTEM_TIMER, true);
    else
        pic_set_mask(IRQ_SYSTEM_TIMER);
}
