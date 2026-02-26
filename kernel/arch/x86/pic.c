#include "pic.h"

#include <log/log.h>
#include <x86/asm.h>
#include <x86/idt.h>

static inline void _wait(void) {
    // Port 0x80 is unused and commonly used for IO wait
    outb(0x80, 0);
}

static void _remap_pic(size_t offset1, size_t offset2) {
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    _wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    _wait();

    outb(PIC1_DATA, (u8)offset1);
    _wait();
    outb(PIC2_DATA, (u8)offset2);
    _wait();

    outb(PIC1_DATA, 4);
    _wait();
    outb(PIC2_DATA, 2);
    _wait();

    outb(PIC1_DATA, ICW4_8086);
    _wait();
    outb(PIC2_DATA, ICW4_8086);
    _wait();
}

void pic_init(void) {
    log_debug("initializing PIC");
    _remap_pic(IRQ_OFFSET, IRQ_OFFSET + 8);
    pic_mask_all();
}

void pic_end_int(size_t irq) {
    if (irq >= 8) {
        outb(PIC2_COMMAND, PIC_EOI);
    }

    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_mask_all(void) {
    outb(PIC1_DATA, 0xff);
    outb(PIC2_DATA, 0xff);
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
