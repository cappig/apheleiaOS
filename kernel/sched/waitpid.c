#include "internal.h"

pid_t sched_wait(pid_t pid, int *status) {
    return sched_waitpid(pid, status, 0);
}

static bool waitpid_target_matches(const sched_thread_t *parent, const sched_thread_t *child, pid_t pid) {
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

        if (waitpid_target_matches(self, thread, pid)) {
            return thread;
        }
    }

    return NULL;
}

static sched_thread_t *waitpid_find_stopped(const sched_thread_t *self, pid_t pid, bool *has_child) {
    ll_foreach(node, sched_state.procs.all_list) {
        sched_thread_t *thread = node->data;

        if (!waitpid_target_matches(self, thread, pid)) {
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

        if (waitpid_target_matches(self, thread, pid)) {
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

static void waitpid_charge_child_time(sched_thread_t *parent, const sched_thread_t *child) {
    if (!parent || !child) {
        return;
    }

    u64 own_ticks = __atomic_load_n(&child->cpu_time_ticks, __ATOMIC_RELAXED);
    u64 child_ticks = __atomic_load_n(&child->child_cpu_time_ticks, __ATOMIC_RELAXED);
    waitpid_add_ticks(&parent->child_cpu_time_ticks, waitpid_sum_ticks(own_ticks, child_ticks));

    u64 own_user = __atomic_load_n(&child->user_ticks, __ATOMIC_RELAXED);
    u64 child_user = __atomic_load_n(&child->child_user_ticks, __ATOMIC_RELAXED);
    waitpid_add_ticks(&parent->child_user_ticks, waitpid_sum_ticks(own_user, child_user));

    u64 own_sys = __atomic_load_n(&child->sys_ticks, __ATOMIC_RELAXED);
    u64 child_sys = __atomic_load_n(&child->child_sys_ticks, __ATOMIC_RELAXED);
    waitpid_add_ticks(&parent->child_sys_ticks, waitpid_sum_ticks(own_sys, child_sys));
}

pid_t sched_waitpid(pid_t pid, int *status, int options) {
    sched_thread_t *self = sched_local_current();

    if (!self || !self->user_thread) {
        return -ECHILD;
    }

    for (;;) {
        sched_thread_t *found = NULL;
        sched_thread_t *stopped = NULL;
        sched_thread_t *active_zombie = NULL;
        bool has_matching_child = false;

        unsigned long flags = sched_lock_save();

        if (!sched_state.procs.zombie_list || !sched_state.procs.all_list) {
            sched_lock_restore(flags);
            return -ECHILD;
        }

        found = waitpid_find_zombie(self, pid);

        if (found && thread_is_owned(found)) {
            active_zombie = found;
            found = NULL;
        }

        if (found) {
            if (status) {
                *status = waitpid_status(found);
            }

            waitpid_charge_child_time(self, found);

            list_remove(sched_state.procs.zombie_list, &found->zombie_node);
            found->in_zombie_list = false;

            sched_lock_restore(flags);

            pid_t ret = found->pid;
            thread_cleanup(found);
            thread_put(found);

            return ret;
        }

        if (options & WUNTRACED) {
            stopped = waitpid_find_stopped(self, pid, &has_matching_child);

            if (stopped) {
                stopped->stop_reported = true;

                if (status) {
                    *status = 0x7f | ((stopped->stop_signal & 0xff) << 8);
                }

                sched_lock_restore(flags);
                return stopped->pid;
            }
        } else {
            has_matching_child = waitpid_has_child(self, pid);
        }

        if (!has_matching_child) {
            sched_lock_restore(flags);
            return -ECHILD;
        }

        if (active_zombie) {
            if (options & WNOHANG) {
                sched_lock_restore(flags);
                return 0;
            }

            sched_lock_restore(flags);

            while (thread_is_owned(active_zombie)) {
                force_resched();
                sched_spin_wait();
            }

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

        sched_wait_result_t wait_result = sched_wait_on_queue(&self->wait_queue, wait_seq, 0, SCHED_WAIT_INTERRUPTIBLE);

        if (wait_result == SCHED_WAIT_INTR) {
            return -EINTR;
        }

        if (wait_result == SCHED_WAIT_ABORTED && sched_running_get()) {
            sched_yield();
        }
    }
}
