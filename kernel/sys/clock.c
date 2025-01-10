#include "sys/clock.h"

#include <log/log.h>
#include <time.h>

#include "arch/cmos.h"
#include "sched/scheduler.h"

static atomic u64 tick_counter = 0;
static u64 boot_time = 0; // In unix milis


void tick_clock() {
    tick_counter++;
    schedule();
}


void clock_init() {
    std_time time = get_time();
    time_t secs_now = mktime(&time);

    boot_time = secs_now * 1000;

    log_info("Current time is: %s [%lu]", asctime(&time), secs_now);
}


u64 clock_now_ms() {
    u64 passed = tick_counter * CLOCK_FREQ;
    return boot_time + passed;
}

time_t clock_now() {
    return clock_now_ms() / 1000;
}
