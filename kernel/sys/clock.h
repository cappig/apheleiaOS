#pragma once

#include <base/types.h>
#include <time.h>

#define CLOCK_FREQ  1000
#define MS_PER_TICK (1000 / CLOCK_FREQ)


void tick_clock(void);

void clock_init(void);

time_t clock_now(void);
