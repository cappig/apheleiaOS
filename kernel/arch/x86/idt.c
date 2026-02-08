#include "idt.h"

#include <base/attributes.h>
#include <base/types.h>
#include <stddef.h>
#include <x86/asm.h>
#include <x86/gdt.h>

static idt_register_t idtr = {0};

extern void* isr_stub_table[ISR_COUNT];
static int_handler_t int_handlers[ISR_COUNT] = {0};

ALIGNED(0x10)
static idt_entry_t idt_entries[ISR_COUNT] = {0};

static void _default_int_handler(int_state_t* state) {
    (void)state;
    disable_interrupts();
    halt();
}

void set_int_handler(size_t int_num, int_handler_t handler) {
    if (int_num >= ISR_COUNT || handler == NULL)
        return;

    int_handlers[int_num] = handler;
}

void configure_int(size_t int_num, u16 selector, u8 ist, u8 attribs) {
    if (int_num >= ISR_COUNT)
        return;

    idt_entry_t* descriptor = &idt_entries[int_num];

    descriptor->selector = selector;
    descriptor->attributes = attribs;

#if defined(__x86_64__)
    descriptor->ist = ist;
#else
    (void)ist;
    descriptor->zero = 0;
#endif
}

void idt_init(void) {
    for (size_t entry = 0; entry < ISR_COUNT; entry++) {
        idt_entry_t* descriptor = &idt_entries[entry];

        uintptr_t stub_ptr = (uintptr_t)isr_stub_table[entry];

        descriptor->offset_low = (u16)(stub_ptr & 0xffff);
        descriptor->selector = GDT_KERNEL_CODE;
        descriptor->attributes = IDT_INT;

#if defined(__x86_64__)
        descriptor->ist = 0;
        descriptor->offset_mid = (u16)((stub_ptr >> 16) & 0xffff);
        descriptor->offset_high = (u32)((stub_ptr >> 32) & 0xffffffff);
        descriptor->reserved0 = 0;
#else
        descriptor->zero = 0;
        descriptor->offset_high = (u16)((stub_ptr >> 16) & 0xffff);
#endif
    }

    idtr.limit = ISR_COUNT * sizeof(idt_entry_t) - 1;
#if defined(__x86_64__)
    idtr.base = (u64)(uintptr_t)idt_entries;
#else
    idtr.base = (u32)(uintptr_t)idt_entries;
#endif

    for (size_t i = 0; i < ISR_COUNT; i++)
        set_int_handler(i, _default_int_handler);

    asm volatile("lidt %0" : : "m"(idtr) : "memory");
}

// Called by isr_common_stub in idt_stubsXX.asm
void isr_handler(int_state_t* state) {
    if (state == NULL) {
        disable_interrupts();
        halt();
    }

    if (state->int_num < ISR_COUNT && int_handlers[state->int_num]) {
        int_handlers[state->int_num](state);
        return;
    }

    disable_interrupts();
    halt();
}
