#include "tsc.h"

#include <log/log.h>
#include <stdlib.h>
#include <sys/panic.h>
#include <x86/asm.h>
#include <x86/pit.h>

#define CAL_MILLIS  10
#define CAL_COUNTER (PIT_BASE_FREQ / (1000 / CAL_MILLIS))
#define CAL_LOOPS   256
#define RETRY_COUNT 4
#define CAL_SAMPLES 5

static u64 tsc_rate_khz = 0;

static bool _has(void) {
    cpuid_regs_t r = {0};
    cpuid(1, &r);

    return (r.edx & (1u << 4)) != 0;
}

// Single PIT-based measurement returning TSC ticks over CAL_MILLIS ms
// Returns 0 on failure (PIT gate count too low)
static u64 _measure_once(void) {
    size_t retry_counter = 0;

retry:
    // Pulse high and disable the PC speaker
    u8 sp = inb(0x61);
    sp &= ~(1u << 1);
    sp |= (1u << 0);
    outb(0x61, sp);

    // Configure PIT channel C
    outb(PIT_CONTROL, 0xb0);
    outb(PIT_C, (u8)(CAL_COUNTER & 0xff));
    outb(PIT_C, (u8)((CAL_COUNTER >> 8) & 0xff));

    u64 begin = read_tsc();

    size_t count = 0;
    while (!(inb(0x61) & (1u << 5))) {
        count++;
    }

    u64 end = read_tsc();

    retry_counter++;

    if (count < CAL_LOOPS) {
        if (retry_counter <= RETRY_COUNT) {
            goto retry;
        }

        return 0;
    }

    return end - begin;
}

static int _cmp_u64(const void *a, const void *b) {
    u64 va = *(const u64 *)a;
    u64 vb = *(const u64 *)b;

    if (va < vb) {
        return -1;
    }
    if (va > vb) {
        return 1;
    }
    return 0;
}

bool tsc_init(void) {
    if (!_has()) {
        log_warn("CPU does not advertise TSC");
        return false;
    }

    // Take multiple samples and pick the median to reject outliers
    // caused by SMIs or other transient delays
    u64 samples[CAL_SAMPLES];
    size_t good = 0;

    for (size_t i = 0; i < CAL_SAMPLES; i++) {
        u64 delta = _measure_once();
        if (delta) {
            samples[good++] = delta;
        }
    }

    if (!good) {
        log_warn("calibration failed (no valid samples)");
        return false;
    }

    qsort(samples, good, sizeof(u64), _cmp_u64);

    u64 median = samples[good / 2];
    tsc_rate_khz = median / CAL_MILLIS;

    log_info(
        "calibrated at %llu MHz (%zu/%d samples)",
        (unsigned long long)(tsc_rate_khz / 1000),
        good,
        CAL_SAMPLES
    );

    return true;
}

u64 tsc_khz(void) {
    return tsc_rate_khz;
}

void tsc_spin(size_t ms) {
    assert(tsc_rate_khz != 0);

    u64 delta = tsc_rate_khz * (u64)ms;
    u64 target = read_tsc() + delta;

    while (read_tsc() < target) {
        continue;
    }
}
