#include "irq.h"

#include <arch/arch.h>
#include <base/attributes.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sys/stats.h>
#include <x86/apic.h>
#include <x86/asm.h>
#include <x86/pic.h>
#include <x86/pit.h>
#include <x86/serial.h>
#include <x86/tsc.h>

#ifndef LEGACY_TIMER_SERIAL_RX
#define LEGACY_TIMER_SERIAL_RX 1
#endif

static volatile u64 irq_tick_count = 0;
static bool use_apic_timer = false;
static bool use_ioapic = false;

static void _route_irqs(bool to_apic) {
    outb(0x22, 0x70);
    u8 imcr = inb(0x23);
    u8 next = to_apic ? (u8)(imcr | 0x01) : (u8)(imcr & ~0x01);
    outb(0x23, next);
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

static void _unregister_legacy(size_t irq) {
#if defined(__i386__)
    if (irq < 8) {
        reset_int_handler(0x08 + irq);
    } else {
        reset_int_handler(0x70 + (irq - 8));
    }
#else
    (void)irq;
#endif
}

static void _timer_handler(int_state_t *state) {
    u64 begin_tsc = read_tsc();
    irq_tick_count++;
    irq_ack(IRQ_SYSTEM_TIMER);

#if LEGACY_TIMER_SERIAL_RX
    for (size_t i = 0; i < 64; i++) {
        char ch = 0;
        if (!serial_try_receive(SERIAL_COM1, &ch)) {
            break;
        }
        if (ch) {
            serial_push_rx(0, ch);
        }
    }
#endif

    sched_tick(state);

    u64 khz = tsc_khz();
    if (khz) {
        u64 delta_tsc = read_tsc() - begin_tsc;
        u64 ns = (delta_tsc * 1000000ULL) / khz;
        stats_add_timer_irq_ns(ns);
    }
}

#if !LEGACY_TIMER_SERIAL_RX
static void _com1_handler(UNUSED int_state_t *state) {
    irq_ack(IRQ_COM1);

    for (size_t i = 0; i < 128; i++) {
        char ch = 0;
        if (!serial_try_receive(SERIAL_COM1, &ch)) {
            break;
        }

        if (ch) {
            serial_push_rx(0, ch);
        }
    }
}
#endif

static void _spurious_handler(UNUSED int_state_t *state) {
}

static void _init_timer_source(bool apic_ok) {
    const u32 timer_hz = TIMER_FREQ ? TIMER_FREQ : 1U;

    use_apic_timer = false;

    if (apic_ok && apic_timer_init(timer_hz)) {
        use_apic_timer = true;
        log_info("APIC timer initialized at %u Hz", apic_timer_hz());
        return;
    }

    pit_set_frequency(timer_hz);
    log_info("PIT timer initialized at %u Hz", pit_get_frequency());
}

bool irq_init(void) {
    bool apic_ok = apic_init();

    if (apic_ok) {
        set_int_handler(INT_SPURIOUS, _spurious_handler);
    }

    if (apic_ok && ioapic_available()) {
        use_ioapic = true;
        ioapic_mask_all();
        pic_mask_all();
        _route_irqs(true);
        log_info("using IOAPIC for external interrupts");
    } else {
        _route_irqs(false);
    }

    _init_timer_source(apic_ok);

    irq_register(IRQ_SYSTEM_TIMER, _timer_handler);
    timer_enable();

#if LEGACY_TIMER_SERIAL_RX
    serial_set_rx_interrupt(SERIAL_COM1, false);
#else
    irq_register(IRQ_COM1, _com1_handler);
    serial_set_rx_interrupt(SERIAL_COM1, true);
#endif

    return true;
}

void irq_register(size_t irq, int_handler_t handler) {
    size_t vec = IRQ_INT(irq);

    if (!handler || vec >= ISR_COUNT) {
        return;
    }

    set_int_handler(vec, handler);
    _register_legacy(irq, handler);

    if (use_ioapic) {
        if (irq != IRQ_SYSTEM_TIMER || !use_apic_timer) {
            u32 dest = lapic_id();
            if (!ioapic_route_irq((u8)irq, (u8)vec, dest)) {
                log_warn("failed to route irq %u via IOAPIC", (u32)irq);
            }
        }
        return;
    }

    if (!use_apic_timer || irq != IRQ_SYSTEM_TIMER) {
        pic_clear_mask((u8)irq);
    }
}

void irq_unregister(size_t irq) {
    size_t vec = IRQ_INT(irq);
    if (vec >= ISR_COUNT) {
        return;
    }

    if (use_ioapic) {
        ioapic_mask_irq((u8)irq, true);
    } else {
        pic_set_mask((u8)irq);
    }

    reset_int_handler(vec);
    _unregister_legacy(irq);
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

u32 irq_timer_hz(void) {
    if (use_apic_timer) {
        return apic_timer_hz();
    }
    return pit_get_frequency();
}

void timer_enable(void) {
    if (use_apic_timer) {
        apic_timer_enable();
        return;
    }

    if (use_ioapic) {
        ioapic_mask_irq(IRQ_SYSTEM_TIMER, false);
    } else {
        pic_clear_mask(IRQ_SYSTEM_TIMER);
    }
}

void timer_disable(void) {
    if (use_apic_timer) {
        apic_timer_disable();
        return;
    }

    if (use_ioapic) {
        ioapic_mask_irq(IRQ_SYSTEM_TIMER, true);
    } else {
        pic_set_mask(IRQ_SYSTEM_TIMER);
    }
}
