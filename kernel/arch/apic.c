#include "apic.h"

#include <base/addr.h>
#include <log/log.h>
#include <x86/asm.h>
#include <x86/paging.h>

#include "arch/idt.h"
#include "arch/irq.h"
#include "arch/panic.h"
#include "arch/tsc.h"
#include "mem/virtual.h"
#include "sys/clock.h"

// The APIC uses memory mapped registers and x2apic uses MSRs
static bool x2_mode = false;
static u64 lapic_vaddr;


// returns -1 if no APIC at all, 1 if X2APIC 0 if regular APIC
static int _apic_available() {
    cpuid_regs r = {0};
    cpuid(1, &r);

    // No APIC present
    if (!(r.edx & (1 << 9)))
        return -1;

    // Is the X2APIC present
    if (r.ecx & (1 << 21))
        return 1;

    return 0;
}


u32 read_lapic(usize reg) {
    if (x2_mode) {
        u32 msr = X2APIC_REGISTER_MSR(reg);
        return read_msr(msr);
    }

    assert(lapic_vaddr);

    volatile u32* addr = (volatile u32*)(lapic_vaddr + reg);
    return *addr;
}

void write_lapic(usize reg, u32 val) {
    if (x2_mode) {
        write_msr(X2APIC_REGISTER_MSR(reg), val);
        return;
    }

    assert(lapic_vaddr);

    volatile u32* addr = (volatile u32*)(lapic_vaddr + reg);
    *addr = val;
}

void lapic_end_int() {
    write_lapic(LAPIC_EOI_REG, 0);
}


u32 read_ioapic(u64 vaddr, u32 reg) {
    u32 volatile* io = (u32 volatile*)vaddr;
    io[0] = (reg & 0xff);

    return io[4];
}

void write_ioapic(u64 vaddr, u32 reg, u32 value) {
    u32 volatile* io = (u32 volatile*)vaddr;

    io[0] = (reg & 0xff);
    io[4] = value;
}


// Redirect the irq to the given interrupt vector on the core with lapic_id
// This means that only one core will be interrupted
void ioapic_map(ioapic* io, u8 vec, u8 irq, u32 lapic_id, bool polarity, bool trigger) {
    u32 num = io->gsi_base + (irq * 2);
    u32 reg = IOAPIC_REG_TBASE + num;

    ioapic_redirection_entry entry = {
        .vector = vec,
        .delivery_mode = IOAPIC_DM_NORMAL,
        .destination_mode = IOAPIC_DEST_PHYSICAL, // TODO: physical mode can only address 16 cores
        .polarity = polarity,
        .trigger_mode = trigger,
        .mask = 1, // Mask the irq for now
        .destination = lapic_id & 0xf
    };

    write_ioapic(io->base_vaddr, reg, entry.raw);
}

void ioapic_clear_mask(ioapic* io, usize irq) {
    u32 num = io->gsi_base + (irq * 2);
    u32 reg = IOAPIC_REG_TBASE + num;

    u32 entry = read_ioapic(io->base_vaddr, reg);
    entry &= ~(1 << 16);
    write_ioapic(io->base_vaddr, reg, entry);
}

void ioapic_set_mask(ioapic* io, usize irq) {
    u32 num = io->gsi_base + (irq * 2);
    u32 reg = IOAPIC_REG_TBASE + num;

    u32 entry = read_ioapic(io->base_vaddr, reg);
    entry |= (1 << 16);
    write_ioapic(io->base_vaddr, reg, entry);
}


// Enable the local APIC for the current core and calibrate the timer
void init_lapic() {
    // Configure the spurious interrupt and flip the enable bit
    write_lapic(LAPIC_SPURIOUS_REG, INT_SPURIOUS | (1 << 8));

    // Configure the timer and calibrate the timer against the TSC
    write_lapic(LAPIC_TIMER_DIVIDE_REG, 3);

    u32 begin = (u32)-1;

    write_lapic(LAPIC_TIMER_ICOUNT_REG, begin);
    tsc_spin(MS_PER_TICK);

    u32 end = read_lapic(LAPIC_TIMER_CCOUNT_REG);
    write_lapic(LAPIC_TIMER_ICOUNT_REG, begin - end);

    // Disable the timer for now
    u32 lvt = IRQ_INT(IRQ_SYSTEM_TIMER) | LAPIC_TIMER_PERIODIC | LAPIC_LVT_MASK;
    write_lapic(LAPIC_LVT_TIMER_REG, lvt);
}

void lapic_enable_timer() {
    u32 lvt = read_lapic(LAPIC_LVT_TIMER_REG);
    lvt &= ~LAPIC_LVT_MASK;
    write_lapic(LAPIC_LVT_TIMER_REG, lvt);
}

void lapic_disable_timer() {
    u32 lvt = read_lapic(LAPIC_LVT_TIMER_REG);
    lvt |= LAPIC_LVT_MASK;
    write_lapic(LAPIC_LVT_TIMER_REG, lvt);
}


bool init_apic() {
    int apic_status = _apic_available();
    if (apic_status == -1)
        return false;

    // Enable the APIC (or the X2APIC if available)
    u64 base = read_msr(APIC_BASE_MSR);

    base |= APIC_MSR_APIC_ENABLE;

    if (apic_status == 1) {
        base |= APIC_MSR_X2APIC_ENABLE;
        x2_mode = true;
    }

    write_msr(APIC_BASE_MSR, base);

    if (!x2_mode) {
        u64 lapic_paddr = base & APIC_MSR_ADDR_MASK;
        lapic_vaddr = ID_MAPPED_VADDR(lapic_paddr);

        u64 flags = PT_PRESENT | PT_WRITE | PT_NO_CACHE;
        map_page((void*)read_cr3(), PAGE_4KIB, lapic_vaddr, lapic_paddr, flags);
    }

    init_lapic();

    log_info("Initialised and enabled the %s", x2_mode ? "X2APIC" : "APIC");

    return true;
}
