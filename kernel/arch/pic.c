#include "pic.h"

#include <base/types.h>
#include <log/log.h>
#include <x86/asm.h>

#include "arch/idt.h"

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
    // Move all IRQs to 32 so that they don't conflict with default exceptions
    remap_pic(IRQ_OFFSET, IRQ_OFFSET + 8);

    // Mask all IRQs by default
    // https://wiki.osdev.org/8259_PIC#Masking
    outb(PIC1_DATA, 0xff);
    outb(PIC2_DATA, 0xff);
}

void pic_end_int(usize irq) {
    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);

    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_set_mask(u8 line) {
    u16 port;

    if (line < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        line -= 8;
    }

    u8 data = inb(port) | (1 << line);
    outb(port, data);
}

void pic_clear_mask(u8 line) {
    u16 port;

    if (line < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        line -= 8;
    }

    u8 data = inb(port) & ~(1 << line);
    outb(port, data);
}
