#pragma once

#include <base/types.h>

#define PIT_A       0x40
#define PIT_B       0x41
#define PIT_C       0x42
#define PIT_CONTROL 0x43

#define PIT_MASK 0xff
#define PIT_SET  0x36

#define PIT_FREQ 100

#define PIT_BASE_FREQ 1193180


void pit_init(void);
