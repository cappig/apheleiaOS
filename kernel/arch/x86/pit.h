#pragma once

#include <base/types.h>

// PIT ports
#define PIT_A       0x40
#define PIT_B       0x41
#define PIT_C       0x42
#define PIT_CONTROL 0x43

#define PIT_SET 0x36

#define PIT_BASE_FREQ  1193180
#ifndef PIT_DEFAULT_HZ
#define PIT_DEFAULT_HZ 100
#endif

void pit_init(void);
void pit_set_frequency(u32 hz);
u32 pit_get_frequency(void);
