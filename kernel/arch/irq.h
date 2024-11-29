#pragma once

#include <base/types.h>

#include "arch/idt.h"

typedef enum {
    IRQ_SYSTEM_TIMER = 0x00,
    IRQ_PS2_KEYBOARD = 0x01,
    IRQ_CASCADE = 0x02,
    IRQ_COM2 = 0x03,
    IRQ_COM1 = 0x04,
    IRQ_LPT2 = 0x05,
    IRQ_FLOPPY = 0x06,
    IRQ_SPURIOUS = 0x07, // ignored
    IRQ_CMOS_RTC = 0x08,
    IRQ_OPEN_9 = 0x09,
    IRQ_OPEN_10 = 0x0a,
    IRQ_OPEN_11 = 0x0b,
    IRQ_PS2_MOUSE = 0x0c,
    IRQ_COPROCESSOR = 0x0d,
    IRQ_PRIMARY_ATA = 0x0e,
    IRQ_SECONDARY_ATA = 0x0f,
} irq_numbers;


bool irq_init(void);

void irq_register(usize irq, int_handler handler);
void irq_ack(usize irq);
