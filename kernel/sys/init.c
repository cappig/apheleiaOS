#include "init.h"

#include <log/log.h>
#include <sched/scheduler.h>

#include "shell.h"

static void _rr_thread(void* arg) {
    (void)arg;
    size_t ticks = 0;

    for (;;) {
        if ((ticks++ % 2048) == 0)
            log_info("sched: rr tick %zu", ticks / 2048);

        for (volatile size_t spin = 0; spin < 2000000; spin++)
            ;

        sched_yield();
    }
}

static void _thread(void* arg) {
    (void)arg;
    log_info("init: starting shell");
    shell_main();

    for (;;)
        sched_yield();
}

void init_spawn(void) {
    if (!sched_create_kernel_thread("init", _thread, NULL))
        log_warn("init: failed to spawn init thread");

    if (!sched_create_kernel_thread("rr", _rr_thread, NULL))
        log_warn("init: failed to spawn rr thread");
}
