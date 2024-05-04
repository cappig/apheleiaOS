#include "idt.h"

#include <base/types.h>
#include <log/log.h>
#include <x86/asm.h>

#include "base/attributes.h"
#include "mem/heap.h"
#include "pic.h"
#include "video/tty.h"

static idt_register idtr;

extern void* isr_stub_table[ISR_COUNT];
static int_handler int_handlers[ISR_COUNT];

ALIGNED(0x10)
static idt_entry idt_entries[ISR_COUNT] = {0};


static void generic_int_handler(int_state* s) {
    log_warn("Unhandled interrupt: [int=%#lx]\n", s->int_num);
}

static void exception_handler(int_state* s) {
    disble_interrupts();
    log_fatal("Unhandled exception: [int=%#lx | error=%#lx]", s->int_num, s->error_code);
    panic("Halting: %s", int_strings[s->int_num]);
}

void idt_init() {
    for (usize vector = 0; vector < ISR_COUNT; vector++) {
        idt_entry* descriptor = &idt_entries[vector];

        u64 stub_ptr = (u64)isr_stub_table[vector];

        descriptor->offset_low = stub_ptr;
        descriptor->selector = 0x08;
        descriptor->attributes = 0x8e;
        descriptor->offset_mid = stub_ptr >> 16;
        descriptor->offset_high = stub_ptr >> 32;
    }

    idtr.limit = ISR_COUNT * sizeof(idt_entry) - 1;
    idtr.base = (u64)idt_entries;

    // Handle the default x86 exceptions
    for (usize exc = 0; exc < INT_COUNT; exc++)
        set_int_handler(exc, exception_handler);

    // Handle all the other possible interrupts
    for (usize trp = INT_COUNT; trp < ISR_COUNT; trp++)
        set_int_handler(trp, generic_int_handler);

    asm volatile("lidt %0" ::"m"(idtr) : "memory");
}

void set_int_handler(usize int_num, int_handler handler) {
    int_handlers[int_num] = handler;
}

// Called by isr_common_stub in idt_stubs.asm
void isr_handler(int_state* s) {
    if (s->int_num >= ISR_COUNT)
        panic("Unknown interrupt number [int=%#lx]", s->int_num);

    int_handlers[s->int_num](s);

    if (IS_IRQ(s->int_num) && s->int_num != IRQ_SPURIOUS)
        pic_end_int(s->int_num - IRQ_OFFSET);
}
