#include "init.h"

#include <log/log.h>
#include <sched/scheduler.h>
#include <sys/exec.h>
#include <sys/tty.h>

void init_spawn(void) {
    log_info("init: spawning /sbin/init");
    sched_thread_t* init = user_spawn("/sbin/init");

    if (!init) {
        log_warn("init: failed to spawn /sbin/init");
        return;
    }

    log_info("init: spawned pid %ld", (long)init->pid);
    init->tty_index = (int)TTY_USER_TO_SCREEN(0);
    tty_set_current(TTY_USER_TO_SCREEN(0));
    sched_make_runnable(init);
    log_debug("init: marked pid %ld runnable", (long)init->pid);
}
