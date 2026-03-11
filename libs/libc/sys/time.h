#pragma once

#ifdef _KERNEL

#include <arch/arch.h>
#include <base/types.h>

static inline u64 ms_to_ticks(u32 timeout_ms) {
    u64 hz = arch_timer_hz();
    if (!hz) {
        return 1;
    }

    u64 ticks = ((hz * timeout_ms) + 999ULL) / 1000ULL;
    return ticks ? ticks : 1;
}

void delay_ms(u32 ms);

#else

#include <sys/types.h>
#include <time.h>

struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

#ifndef _KERNEL
int gettimeofday(struct timeval *restrict tv, struct timezone *restrict tz);
#endif

#endif
