#include "init.h"

#include <log/log.h>
#include <sched/scheduler.h>
#include <sys/exec.h>
#include <sys/tty.h>

void init_spawn(void) {
    log_debug("spawning /bin/init");
    sched_thread_t *init = user_spawn("/bin/init");

    if (!init) {
        log_warn("failed to spawn /bin/init");
        return;
    }

    log_debug("spawned pid %ld", (long)init->pid);

    init->tty_index = (int)TTY_USER_TO_SCREEN(0);
    tty_set_current(TTY_USER_TO_SCREEN(0));
    sched_make_runnable(init);

    // log_debug("marked pid %ld runnable", (long)init->pid);
}
