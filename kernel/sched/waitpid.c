#include "internal.h"

pid_t sched_wait(pid_t pid, int *status) {
    return sched_waitpid(pid, status, 0);
}

static bool waitpid_matches(const sched_thread_t *parent, const sched_thread_t *child, pid_t pid) {
    if (!parent || !child || !child->user_thread) {
        return false;
    }

    if (child->ppid != parent->pid) {
        return false;
    }

    if (pid > 0) {
        return child->pid == pid;
    }

    if (!pid) {
        // pid 0 waits within the caller's process group
        return child->pgid == parent->pgid;
    }

    if (pid == -1) {
        return true;
    }

    return child->pgid == -pid;
}

static sched_thread_t *waitpid_find_zombie(const sched_thread_t *self, pid_t pid) {
    ll_foreach(node, sched_state.procs.zombie_list) {
        sched_thread_t *thread = node->data;

        if (waitpid_matches(self, thread, pid)) {
            return thread;
        }
    }

    return NULL;
}

static sched_thread_t *waitpid_find_stopped(const sched_thread_t *self, pid_t pid, bool *has_child) {
    ll_foreach(node, sched_state.procs.all_list) {
        sched_thread_t *thread = node->data;

        if (!waitpid_matches(self, thread, pid)) {
            continue;
        }

        if (has_child) {
            *has_child = true;
        }

        bool stopped_child = (thread_get_state(thread) == THREAD_STOPPED && !thread->stop_reported);

        if (stopped_child) {
            return thread;
        }
    }

    return NULL;
}

static bool waitpid_has_child(const sched_thread_t *self, pid_t pid) {
    ll_foreach(node, sched_state.procs.all_list) {
        sched_thread_t *thread = node->data;

        if (waitpid_matches(self, thread, pid)) {
            return true;
        }
    }

    return false;
}

static int waitpid_status(const sched_thread_t *thread) {
    if (thread->exit_signal) {
        return thread->exit_signal & 0x7f;
    }

    int code = thread->exit_code;

    if (code < 0) {
        code = 1;
    }

    return (code & 0xff) << 8;
}

static void waitpid_add_ticks(u64 *counter, u64 ticks) {
    if (!counter || !ticks) {
        return;
    }

    u64 current = *counter;
    u64 next = current + ticks;

    if (next < current) {
        next = UINT64_MAX;
    }

    *counter = next;
}

static u64 waitpid_sum_ticks(u64 own_ticks, u64 child_ticks) {
    u64 total_ticks = own_ticks + child_ticks;

    if (total_ticks < own_ticks) {
        return UINT64_MAX;
    }

    return total_ticks;
}

static void _charge_child_time(sched_thread_t *parent, const sched_thread_t *child) {
    if (!parent || !child) {
        return;
    }

    u64 own_ticks = __atomic_load_n(&child->cpu_time_ticks, __ATOMIC_RELAXED);
    u64 child_ticks = __atomic_load_n(&child->child_cpu_time_ticks, __ATOMIC_RELAXED);
    // waited children roll their accumulated descendants into the parent totals
    waitpid_add_ticks(&parent->child_cpu_time_ticks, waitpid_sum_ticks(own_ticks, child_ticks));

    u64 own_user = __atomic_load_n(&child->user_ticks, __ATOMIC_RELAXED);
    u64 child_user = __atomic_load_n(&child->child_user_ticks, __ATOMIC_RELAXED);
    waitpid_add_ticks(&parent->child_user_ticks, waitpid_sum_ticks(own_user, child_user));

    u64 own_sys = __atomic_load_n(&child->sys_ticks, __ATOMIC_RELAXED);
    u64 child_sys = __atomic_load_n(&child->child_sys_ticks, __ATOMIC_RELAXED);
    waitpid_add_ticks(&parent->child_sys_ticks, waitpid_sum_ticks(own_sys, child_sys));
}

typedef struct {
    sched_thread_t *zombie;
    sched_thread_t *stopped;
    sched_thread_t *active_zombie;
    bool has_child;
} waitpid_scan_t;

static waitpid_scan_t waitpid_scan(sched_thread_t *self, pid_t pid, int options) {
    waitpid_scan_t scan = { 0 };

    scan.zombie = waitpid_find_zombie(self, pid);
    if (scan.zombie && thread_is_owned(scan.zombie)) {
        scan.active_zombie = scan.zombie;
        scan.zombie = NULL;
    }

    if (scan.zombie) {
        scan.has_child = true;
        return scan;
    }

    if (options & WUNTRACED) {
        scan.stopped = waitpid_find_stopped(self, pid, &scan.has_child);
    } else {
        scan.has_child = waitpid_has_child(self, pid);
    }

    return scan;
}

static pid_t reap_zombie(sched_thread_t *self, sched_thread_t *zombie, int *status, unsigned long flags) {
    if (status) {
        *status = waitpid_status(zombie);
    }

    _charge_child_time(self, zombie);

    list_remove(sched_state.procs.zombie_list, &zombie->zombie_node);
    zombie->in_zombie_list = false;

    sched_lock_restore(flags);

    pid_t child_pid = zombie->pid;
    thread_cleanup(zombie);
    thread_put(zombie);

    return child_pid;
}

static pid_t report_stopped(sched_thread_t *thread, int *status, unsigned long flags) {
    thread->stop_reported = true;

    if (status) {
        *status = 0x7f | ((thread->stop_signal & 0xff) << 8);
    }

    sched_lock_restore(flags);
    return thread->pid;
}

static void wait_for_zombie_owner(sched_thread_t *thread) {
    while (thread_is_owned(thread)) {
        force_resched();
        sched_spin_wait();
    }
}

pid_t sched_waitpid(pid_t pid, int *status, int options) {
    sched_thread_t *self = sched_local_current();

    if (!self || !self->user_thread) {
        return -ECHILD;
    }

    if (options & ~(WNOHANG | WUNTRACED)) {
        return -EINVAL;
    }

    for (;;) {
        unsigned long flags = sched_lock_save();

        if (!sched_state.procs.zombie_list || !sched_state.procs.all_list) {
            sched_lock_restore(flags);
            return -ECHILD;
        }

        waitpid_scan_t scan = waitpid_scan(self, pid, options);
        if (scan.zombie) {
            return reap_zombie(self, scan.zombie, status, flags);
        }

        if (scan.stopped) {
            return report_stopped(scan.stopped, status, flags);
        }

        if (!scan.has_child) {
            sched_lock_restore(flags);
            return -ECHILD;
        }

        if (scan.active_zombie) {
            if (options & WNOHANG) {
                sched_lock_restore(flags);
                return 0;
            }

            sched_lock_restore(flags);
            wait_for_zombie_owner(scan.active_zombie);
            continue;
        }

        if (options & WNOHANG) {
            sched_lock_restore(flags);
            return 0;
        }

        if (!sched_running_get()) {
            sched_lock_restore(flags);
            return -ECHILD;
        }

        u32 wait_seq = sched_wait_seq(&self->wait_queue);
        sched_lock_restore(flags);

        if (!arch_timer_ticks()) {
            arch_cpu_wait();
            sched_yield();
            continue;
        }

        sched_wait_result_t wait_result = sched_wait_on(&self->wait_queue, wait_seq, 0, SCHED_WAIT_INTERRUPTIBLE);

        if (wait_result == SCHED_WAIT_INTR) {
            return -EINTR;
        }

        if (wait_result == SCHED_WAIT_ABORTED && sched_running_get()) {
            sched_yield();
        }
    }
}
