#include "arch/irq.h"

#include <base/addr.h>
#include <data/list.h>
#include <log/log.h>
#include <x86/asm.h>

#include "arch/apic.h"
#include "arch/idt.h"
#include "arch/pic.h"
#include "arch/pit.h"
#include "drivers/acpi.h"
#include "mem/heap.h"
#include "mem/virtual.h"
#include "sys/clock.h"
#include "sys/cpu.h"
#include "sys/panic.h"

static bool has_apic = false;
static linked_list* ioapics = NULL;


static void spurious_handler(UNUSED int_state* s) {
    log_warn("Received spurious interrupt!");
}

static void timer_handler(UNUSED int_state* s) {
    tick_clock();
    irq_ack(IRQ_SYSTEM_TIMER);
}


static ioapic* _get_authoritative_apic(usize irq) {
    foreach (node, ioapics) {
        ioapic* io = node->data;

        if (irq >= io->gsi_base && irq < io->gsi_base + io->int_count)
            return io;
    }

    return NULL;
}

static void _init_legacy(void) {
    has_apic = false;
    pit_init();
}

static bool _init_advanced(void) {
    madt* apic = (madt*)(uptr)acpi_find_table("APIC");
    if (!apic)
        return false;

    ioapics = list_create();

    usize offset = 0;
    while (offset < apic->header.length - sizeof(madt)) {
        madt_entry* entry = (madt_entry*)(apic->entries + offset);

        if (entry->type == MT_IO_APIC) {
            ioapic* io = kcalloc(sizeof(ioapic));

            io->id = entry->data[0];
            io->gsi_base = *(u32*)&entry->data[6];

            u32 paddr = *(u32*)&entry->data[2];
            io->base_vaddr = ID_MAPPED_VADDR(paddr);

            u64 flags = PT_PRESENT | PT_WRITE | PT_NO_CACHE;
            map_page((void*)read_cr3(), PAGE_4KIB, io->base_vaddr, paddr, flags);

            u32 ver_reg = read_ioapic(io->base_vaddr, IOAPIC_REG_VER);
            io->int_count = (ver_reg >> 16) + 1;

            // Mask all the interrupts by default
            for (usize i = 0; i < io->int_count; i++)
                ioapic_set_mask(io, i);

            list_append(ioapics, list_create_node(io));

#ifdef INT_DEBUG
            log_debug("[INT_DEBUG] New IOAPIC: id=%u base=%#x gsi=%#x", io->id, paddr, io->gsi_base);
#endif
        }

        offset += entry->length;
    }

    return true;
}


bool irq_init() {
    has_apic = init_apic();
    if (!has_apic)
        goto fallback;

    if (!_init_advanced())
        goto fallback;

    set_int_handler(0xff, spurious_handler);
    irq_register(IRQ_SYSTEM_TIMER, timer_handler);

    log_info("Configured APIC IRQs");
    return true;

fallback:
    _init_legacy();

    log_warn("APIC not detected! Fell back to the 8259 PIC");
    return false;
}


void irq_register(usize irq, int_handler handler) {
    usize vec = irq + IRQ_OFFSET;
    assert(vec <= 0xff);

    set_int_handler(vec, handler);

    if (has_apic) {
        ioapic* io = _get_authoritative_apic(irq);
        assert(io != NULL);

        ioapic_map(io, vec, irq, cpu->lapic_id, IOAPIC_POL_LOW, IOAPIC_TM_EDGE);
        ioapic_clear_mask(io, irq);
    } else {
        pic_clear_mask(irq);
    }

#ifdef INT_DEBUG
    log_debug("[INT_DEBUG] Registered a new handler for IRQ %#zx", irq);
#endif
}

void irq_ack(usize irq) {
    if (has_apic)
        lapic_end_int();
    else
        pic_end_int(irq);

#ifdef INT_DEBUG
    log_debug("[INT_DEBUG] Sent EOI for IRQ %#zx", irq);
#endif
}


void timer_enable() {
    if (has_apic)
        lapic_enable_timer();
}

void timer_disable() {
    if (has_apic)
        lapic_disable_timer();
    else
        pic_set_mask(IRQ_SYSTEM_TIMER);
}
