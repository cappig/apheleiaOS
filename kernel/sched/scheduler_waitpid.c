#include "scheduler_internal.h"

pid_t sched_wait(pid_t pid, int *status) {
    return sched_waitpid(pid, status, 0);
}

static bool waitpid_target_matches(
    const sched_thread_t *parent,
    const sched_thread_t *child,
    pid_t pid
) {
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

pid_t sched_waitpid(pid_t pid, int *status, int options) {
    sched_thread_t *self = sched_local_current();
    if (!self || !self->user_thread) {
        return -ECHILD;
    }

    for (;;) {
        sched_thread_t *found = NULL;
        sched_thread_t *stopped = NULL;
        bool has_matching_child = false;

        unsigned long flags = sched_lock_save();

        if (!sched_state.zombie_list || !sched_state.all_list) {
            sched_lock_restore(flags);
            return -ECHILD;
        }

        ll_foreach(node, sched_state.zombie_list) {
            sched_thread_t *thread = node->data;
            if (!waitpid_target_matches(self, thread, pid)) {
                continue;
            }

            found = thread;
            break;
        }

        if (found) {
            if (status) {
                *status = found->exit_code;
            }

            list_remove(sched_state.zombie_list, &found->zombie_node);
            found->in_zombie_list = false;

            sched_lock_restore(flags);

            pid_t ret = found->pid;
            remove_all_thread(found);
            sched_thread_put(found);
            return ret;
        }

        if (options & WUNTRACED) {
            ll_foreach(node, sched_state.all_list) {
                sched_thread_t *thread = node->data;

                if (!waitpid_target_matches(self, thread, pid)) {
                    continue;
                }

                has_matching_child = true;

                if (
                    sched_thread_state_load(thread) != THREAD_STOPPED ||
                    thread->stop_reported
                ) {
                    continue;
                }

                stopped = thread;
                break;
            }

            if (stopped) {
                stopped->stop_reported = true;
                if (status) {
                    *status = 0x7f | ((stopped->stop_signal & 0xff) << 8); // as per POSIX
                }
                sched_lock_restore(flags);
                return stopped->pid;
            }
        } else {
            ll_foreach(node, sched_state.all_list) {
                sched_thread_t *thread = node->data;
                if (waitpid_target_matches(self, thread, pid)) {
                    has_matching_child = true;
                    break;
                }
            }
        }

        if (!has_matching_child) {
            sched_lock_restore(flags);
            return -ECHILD;
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
        sched_wait_result_t wait_result = sched_wait_on_queue(
            &self->wait_queue,
            wait_seq,
            0,
            SCHED_WAIT_INTERRUPTIBLE
        );
        if (wait_result == SCHED_WAIT_INTR) {
            return -EINTR;
        }
    }
}
