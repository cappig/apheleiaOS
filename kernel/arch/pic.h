#pragma once

#include <base/types.h>

#define PIC1         0x20
#define PIC1_COMMAND PIC1
#define PIC1_DATA    (PIC1 + 1)

#define PIC2         0xa0
#define PIC2_COMMAND PIC2
#define PIC2_DATA    (PIC2 + 1)

#define ICW4_8086 0x01
#define ICW1_ICW4 0x01
#define ICW1_INIT 0x10

#define PIC_EOI 0x20


void pic_init(void);

void pic_end_int(usize irq);

void pic_set_mask(u8 line);
void pic_clear_mask(u8 line);
