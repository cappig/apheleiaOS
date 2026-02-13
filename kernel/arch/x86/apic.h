#pragma once

#include <base/types.h>
#include <stdbool.h>

#define APIC_BASE_MSR 0x1b

#define APIC_MSR_ADDR_MASK (~0xfffULL)

#define LAPIC_LVT_MASK       (1U << 16)
#define LAPIC_TIMER_PERIODIC (1U << 17)

enum cpuid_feature_edx_flags {
    CPUID_FEAT_EDX_MSR = 1U << 5,
    CPUID_FEAT_EDX_APIC = 1U << 9,
};

enum apic_base_msr_flags {
    APIC_MSR_IS_BSP = 1U << 8,
    APIC_MSR_X2APIC_ENABLE = 1U << 10,
    APIC_MSR_APIC_ENABLE = 1U << 11,
};

enum lapic_spurious_flags {
    LAPIC_SPURIOUS_SW_ENABLE = 1U << 8,
};

enum lapic_id_register_fields {
    LAPIC_ID_SHIFT = 24,
    LAPIC_ID_MASK = 0xffU,
};

enum madt_lapic_flags {
    MADT_LAPIC_FLAG_ENABLED = 1U << 0,
};

enum lapic_registers {
    LAPIC_ID_REG = 0x20,
    LAPIC_VERSION_REG = 0x30,
    LAPIC_TPR_REG = 0x80,
    LAPIC_APR_REG = 0x90,
    LAPIC_PPR_REG = 0xa0,
    LAPIC_EOI_REG = 0xb0,
    LAPIC_RRD_REG = 0xc0,
    LAPIC_LD_REG = 0xd0,
    LAPIC_DF_REG = 0xe0,
    LAPIC_SPURIOUS_REG = 0xf0,

    LAPIC_ISR_REG = 0x100,
    LAPIC_TMR_REG = 0x180,
    LAPIC_IRR_REG = 0x200,
    LAPIC_ERROR_REG = 0x280,
    LAPIC_CMCI_REG = 0x2f0,
    LAPIC_ICR_REG = 0x300,

    LAPIC_LVT_TIMER_REG = 0x320,
    LAPIC_LVT_THERMAL_REG = 0x330,
    LAPIC_LVT_PMC_REG = 0x340,
    LAPIC_LVT_LINT0_REG = 0x350,
    LAPIC_LVT_LINT1_REG = 0x360,
    LAPIC_LVT_ERROR_REG = 0x370,

    LAPIC_TIMER_ICOUNT_REG = 0x380,
    LAPIC_TIMER_CCOUNT_REG = 0x390,
    LAPIC_TIMER_DIVIDE_REG = 0x3e0,
};

bool apic_init(void);
bool apic_timer_init(u32 hz);
void apic_timer_enable(void);
void apic_timer_disable(void);
bool apic_timer_active(void);
u32 apic_timer_hz(void);

bool ioapic_available(void);
void ioapic_mask_all(void);
void ioapic_mask_irq(u8 irq, bool masked);
bool ioapic_route_irq(u8 irq, u8 vector, u32 dest_apic);

void lapic_end_int(void);
u32 lapic_id(void);
