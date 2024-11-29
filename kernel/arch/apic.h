#pragma once

#include <base/types.h>
#include <data/list.h>

#define APIC_BASE_MSR   0x1b
#define X2APIC_MSR_BASE 0x800

#define APIC_MSR_ADDR_MASK (~0xfff)

#define X2APIC_REGISTER_MSR(offset) (((offset) >> 4) + X2APIC_MSR_BASE)

#define LAPIC_LVT_MASK       (1 << 16)
#define LAPIC_TIMER_PERIODIC (1 << 17)

// How many milis should one tick last
#define APIC_TIMER_MS 10

// Figure 12-26. IA32_APIC_BASE MSR
enum apic_base_msr_flags {
    APIC_MSR_IS_BSP = 1 << 8,
    APIC_MSR_X2APIC_ENABLE = 1 << 10,
    APIC_MSR_APIC_ENABLE = 1 << 11,
};

// https://wiki.osdev.org/APIC#Local_APIC_registers
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

// https://wiki.osdev.org/APIC#IO_APIC_Registers
enum ioapic_registers {
    IOAPIC_REG_ID = 0x00,
    IOAPIC_REG_VER = 0x01,
    IOAPIC_REG_ARB = 0x02,
    IOAPIC_REG_TBASE = 0x10,
};

typedef union {
    struct {
        u64 vector           : 8;
        u64 delivery_mode    : 3;
        u64 destination_mode : 1;
        u64 delivery_status  : 1;
        u64 polarity         : 1;
        u64 received_lvl     : 1;
        u64 trigger_mode     : 1;
        u64 mask             : 1;
        u64 _unused0         : 39;
        u64 destination      : 8;
    };
    u32 raw;
} ioapic_redirection_entry;

#define IOAPIC_TM_EDGE  0
#define IOAPIC_TM_LEVEL 1

#define IOAPIC_DEST_PHYSICAL 0
#define IOAPIC_DEST_LOGICAL  1

#define IOAPIC_POL_HIGH 1
#define IOAPIC_POL_LOW  0

enum ioapic_dm {
    IOAPIC_DM_NORMAL = 0,
    IOAPIC_DM_LOW = 1,
    IOAPIC_DM_SMINT = 2,
    IOAPIC_DM_NMI = 4,
    IOAPIC_DM_INIT = 5,
    IOAPIC_DM_EXTERNAL = 7,
};

typedef struct {
    u8 id;

    u32 gsi_base;
    u8 int_count;

    u64 base_vaddr;
} ioapic;


u32 read_lapic(usize reg);
void write_lapic(usize reg, u32 val);

void lapic_end_int(void);

u32 read_ioapic(u64 vaddr, u32 reg);
void write_ioapic(u64 vaddr, u32 reg, u32 value);

void ioapic_map(ioapic* io, u8 vec, u8 irq, u32 lapic_id, bool polarity, bool trigger);
void ioapic_clear_mask(ioapic* io, usize irq);
void ioapic_set_mask(ioapic* io, usize irq);

void init_lapic(void);
void lapic_enable_timer(void);
void lapic_disable_timer(void);

bool init_apic(void);
