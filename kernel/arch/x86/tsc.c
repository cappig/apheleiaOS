#include "tsc.h"

#include <log/log.h>
#include <sys/panic.h>
#include <x86/asm.h>
#include <x86/pit.h>

#define CAL_MILLIS  10
#define CAL_COUNTER (PIT_BASE_FREQ / (1000 / CAL_MILLIS))
#define CAL_LOOPS   256
#define RETRY_COUNT 4

static u64 tsc_rate_khz = 0;

static bool _has(void) {
    cpuid_regs_t r = {0};
    cpuid(1, &r);

    return (r.edx & (1u << 4)) != 0;
}

bool tsc_init(void) {
    if (!_has()) {
        log_warn("tsc: CPU does not advertise TSC");
        return false;
    }

    size_t retry_counter = 0;

retry:
    // Pulse high and disable the PC speaker.
    u8 sp = inb(0x61);
    sp &= ~(1u << 1);
    sp |= (1u << 0);
    outb(0x61, sp);

    // Configure PIT channel C.
    outb(PIT_CONTROL, 0xb0);
    outb(PIT_C, (u8)(CAL_COUNTER & 0xff));
    outb(PIT_C, (u8)((CAL_COUNTER >> 8) & 0xff));

    u64 begin = read_tsc();

    size_t count = 0;
    while (!(inb(0x61) & (1u << 5)))
        count++;

    u64 end = read_tsc();

    retry_counter++;

    if (count < CAL_LOOPS) {
        if (retry_counter <= RETRY_COUNT)
            goto retry;

        log_warn("tsc: calibration failed");
        return false;
    }

    u64 delta = end - begin;
    tsc_rate_khz = delta / CAL_MILLIS;

    log_info("tsc: calibrated at %llu MHz", (unsigned long long)(tsc_rate_khz / 1000));

    const char* try_str = (retry_counter > 1) ? "tries" : "try";
    log_debug("tsc: calibration took %zu %s", retry_counter, try_str);

    return true;
}

u64 tsc_khz(void) {
    return tsc_rate_khz;
}

void tsc_spin(size_t ms) {
    assert(tsc_rate_khz != 0);

    u64 delta = tsc_rate_khz * (u64)ms;
    u64 target = read_tsc() + delta;

    while (read_tsc() < target)
        continue;
}
