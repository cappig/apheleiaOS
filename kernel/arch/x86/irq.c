#include "irq.h"

void arch_wallclock_maintain(void); // x86-internal, defined in arch.c

#include <arch/arch.h>
#include <base/attributes.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sys/cpu.h>
#include <x86/apic.h>
#include <x86/asm.h>
#include <x86/pic.h>
#include <x86/pit.h>
#include <x86/serial.h>
#include <x86/smp.h>

#ifndef LEGACY_TIMER_SERIAL_RX
#define LEGACY_TIMER_SERIAL_RX 1
#endif

static volatile u64 irq_tick_count ALIGNED(8) = 0;
static volatile u64 irq_core_tick_count[MAX_CORES] ALIGNED(8) = {0};
static bool use_apic_timer = false;
static bool use_ioapic = false;

static inline void _align_core_tick_floor(size_t cpu_id) {
    if (cpu_id >= MAX_CORES) {
        return;
    }

    u64 global = __atomic_load_n(&irq_tick_count, __ATOMIC_ACQUIRE);
    u64 local = __atomic_load_n(&irq_core_tick_count[cpu_id], __ATOMIC_RELAXED);
    while (local < global) {
        if (__atomic_compare_exchange_n(
                &irq_core_tick_count[cpu_id],
                &local,
                global,
                false,
                __ATOMIC_RELEASE,
                __ATOMIC_RELAXED
            )) {
            return;
        }
    }
}

static inline void _publish_tick(size_t cpu_id) {
    if (cpu_id >= MAX_CORES) {
        cpu_id = 0;
    }

    u64 core_ticks =
        __atomic_add_fetch(&irq_core_tick_count[cpu_id], 1, __ATOMIC_RELAXED);
    u64 observed = __atomic_load_n(&irq_tick_count, __ATOMIC_RELAXED);

    while (core_ticks > observed) {
        if (__atomic_compare_exchange_n(
                &irq_tick_count,
                &observed,
                core_ticks,
                false,
                __ATOMIC_RELEASE,
                __ATOMIC_RELAXED
            )) {
            break;
        }
    }
}

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
    cpu_core_t *core = cpu_current();
    size_t cpu_id = (core && core->id < MAX_CORES) ? core->id : 0;

    _publish_tick(cpu_id);
    if (cpu_id == 0) {
        arch_wallclock_maintain();
    }
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

static void _soft_resched_handler(int_state_t *state) {
    sched_resched_softirq(state);
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

    set_int_handler(SCHED_SOFT_RESCHED_VECTOR, _soft_resched_handler);

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

void irq_init_ap(void) {
    cpu_core_t *core = cpu_current();
    size_t cpu_id = (core && core->id < MAX_CORES) ? core->id : 0;
    _align_core_tick_floor(cpu_id);

    if (!apic_timer_init_local()) {
        return;
    }

    apic_timer_enable();
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
    return __atomic_load_n(&irq_tick_count, __ATOMIC_ACQUIRE);
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
