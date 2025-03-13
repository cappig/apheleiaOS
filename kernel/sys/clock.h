#pragma once

#include <base/macros.h>
#include <base/types.h>
#include <time.h>

#define CLOCK_FREQ  100
#define MS_PER_TICK (1000 / CLOCK_FREQ)

static inline usize ms_to_ticks(usize ms) {
    return DIV_ROUND_UP(ms, MS_PER_TICK);
}

static inline usize ticks_to_ms(usize ticks) {
    return ticks * MS_PER_TICK;
}


void tick_clock(void);

void clock_init(void);

u64 clock_now_ms(void);
time_t clock_now(void);
