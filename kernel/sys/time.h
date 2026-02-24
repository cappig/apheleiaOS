#pragma once

#include <arch/arch.h>
#include <base/types.h>

static inline u64 ms_to_ticks(u32 timeout_ms) {
    u64 hz = arch_timer_hz();
    if (!hz) {
        return 1;
    }

    // round up so short deadlines never collapse to zero ticks
    u64 ticks = ((hz * timeout_ms) + 999ULL) / 1000ULL;
    return ticks ? ticks : 1;
}

void delay_ms(u32 ms);
