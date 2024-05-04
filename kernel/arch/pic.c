#include "pic.h"

#include <base/types.h>
#include <x86/asm.h>

#include "idt.h"
#include "log/log.h"


static void set_timer_freq(usize hz) {
    u16 divisor = (u16)(PIT_BASE_FREQ / hz);

    outb(PIT_CONTROL, PIT_SET);
    outb(PIT_A, divisor & PIT_MASK);
    outb(PIT_A, (divisor >> 8) & PIT_MASK);
}

static void irq_sys_timer(UNUSED int_state* s) {
    log_warn(".irq_timer.");
}

static void remap_pic(usize offset1, usize offset2) {
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);

    outb(PIC1_DATA, offset1);
    outb(PIC2_DATA, offset2);

    outb(PIC1_DATA, 4);
    outb(PIC2_DATA, 2);

    outb(PIC1_DATA, ICW4_8086);
    outb(PIC2_DATA, ICW4_8086);
}

void pic_init() {
    remap_pic(IRQ_OFFSET, IRQ_OFFSET + 8);

    // Mask all IRQs except the PS2 keyboard
    // https://wiki.osdev.org/8259_PIC#Masking
    outb(PIC1_DATA, 0xFF & ~(1 << IRQ_PS2_KEYBOARD));
    outb(PIC2_DATA, 0xFF);

    // Set up the system timer irq
    set_timer_freq(100);

    set_int_handler(IRQ_NUMBER(IRQ_SYSTEM_TIMER), irq_sys_timer);
}

void pic_end_int(usize irq) {
    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);

    outb(PIC1_COMMAND, PIC_EOI);
}
