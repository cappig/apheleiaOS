#include "time.h"

#include <sched/scheduler.h>

void delay_ms(u32 ms) {
    if (!ms) {
        return;
    }

    u64 start = arch_timer_ticks();
    u64 timeout = ms_to_ticks(ms);

    while ((arch_timer_ticks() - start) < timeout) {
        if (sched_is_running() && sched_current()) {
            u64 elapsed = arch_timer_ticks() - start;
            u64 remaining = timeout > elapsed ? (timeout - elapsed) : 0;
            if (!remaining) {
                break;
            }
            sched_sleep(remaining);
            continue;
        }

        arch_cpu_relax();
    }
}
