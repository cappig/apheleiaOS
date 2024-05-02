#pragma once

#include <base/types.h>

#define PIC1         0x20
#define PIC1_COMMAND PIC1
#define PIC1_DATA    (PIC1 + 1)

#define PIC2         0xA0
#define PIC2_COMMAND PIC2
#define PIC2_DATA    (PIC2 + 1)

#define ICW4_8086 0x01
#define ICW1_ICW4 0x01
#define ICW1_INIT 0x10

#define PIC_EOI 0x20

#define PIT_A       0x40
#define PIT_B       0x41
#define PIT_C       0x42
#define PIT_CONTROL 0x43

#define PIT_MASK 0xFF
#define PIT_SET  0x36

#define PIT_BASE_FREQ 1193180

void pic_init(void);

void pic_end_int(usize irq);
