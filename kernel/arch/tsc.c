#include "tsc.h"

#include <log/log.h>
#include <x86/asm.h>

#include "arch/pit.h"
#include "sys/panic.h"

#define CAL_MILIS   10
#define CAL_COUNTER (PIT_BASE_FREQ / (1000 / CAL_MILIS))
#define CAL_LOOPS   256

#define RETRY_COUNT 4

static u64 tsc_khz = 0;

static bool _has_tsc(void) {
    cpuid_regs r = {0};
    cpuid(1, &r);

    return (r.edx & (1 << 4));
}


// This code is based on the linux implementation in linux/arch/x86/kernel/tsc.c
// Estimate the core frequency by measuring the tsm delta against the PIT
// Is the TSC system critical? should we panic on error?
void calibrate_tsc() {
    if (!_has_tsc())
        panic("CPU doesn't have a TSC");

    usize retry_counter = 0;

retry:
    // Pulse high and disable the PC speaker
    u8 sp = inb(0x61);
    sp &= ~(1 << 1);
    sp |= (1 << 0);
    outb(0x61, sp);

    // Configure the PIT
    outb(PIT_CONTROL, 0xb0);
    outb(PIT_C, CAL_COUNTER & 0xff);
    outb(PIT_C, (CAL_COUNTER >> 8) & 0xff);

    u64 begin = read_tsc();

    // Count to CAL_MILIS
    usize count = 0;
    while (!(inb(0x61) & (1 << 5)))
        count++;

    u64 end = read_tsc();

    retry_counter++;

    if (count < CAL_LOOPS) {
        if (retry_counter <= RETRY_COUNT)
            goto retry;

        panic("Unable to calibrate the TSC");
    }

    u64 delta = end - begin;
    tsc_khz = delta / CAL_MILIS;

    // disable the PIT?

    log_info("Initialised %lu MHz TSC", tsc_khz / 1000);

    char* try_str = (retry_counter > 1) ? "tries" : "try";
    log_debug("Took %zu %s to calibrate the TSC", retry_counter, try_str);
}


// Spin in a loop for a given number of milliseconds
// This can be used as a crude sleep() alternative in the kernel
void tsc_spin(usize ms) {
    assert(tsc_khz != 0);

    u64 delta = tsc_khz * ms;
    u64 target = read_tsc() + delta;

    while (read_tsc() < target)
        continue;
}
