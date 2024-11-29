#pragma once

#include <base/types.h>

// https://wiki.osdev.org/PC_Speaker#Through_the_Programmable_Interval_Timer_(PIT)
// https://wiki.osdev.org/Programmable_Interval_Timer

// Channel A is hooked up to IRQ0
// Channel B is obsolete
// Channel C can be hooked up to the PC speaker
#define PIT_A       0x40
#define PIT_B       0x41
#define PIT_C       0x42
#define PIT_CONTROL 0x43

#define PIT_MASK 0xff
#define PIT_SET  0x36

#define PIT_FREQ 100

#define PIT_BASE_FREQ 1193180


void pit_init(void);
