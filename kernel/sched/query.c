#include "internal.h"

bool sched_proc_snapshot(pid_t pid, sched_proc_snapshot_t *out) {
    if (!out || pid < 0 || !sched_state.all_list) {
        return false;
    }

    u64 hz = arch_timer_hz();

    bool found = false;
    unsigned long flags = sched_lock_save();
    sched_thread_t *thread = pid_get(pid);

    if (!thread) {
        ll_foreach(node, sched_state.all_list) {
            sched_thread_t *candidate = node->data;

            if (!candidate || candidate->pid != pid) {
                continue;
            }

            thread = candidate;
            pid_set(thread);

            break;
        }
    }

    if (thread) {
        out->pid = thread->pid;
        out->ppid = thread->ppid;
        out->pgid = thread->pgid;
        out->sid = thread->sid;
        out->uid = thread->uid;
        out->gid = thread->gid;
        out->umask = thread->umask & 0777;
        out->signal_pending =
            __atomic_load_n(&thread->signal_pending, __ATOMIC_ACQUIRE);
        out->signal_mask =
            __atomic_load_n(&thread->signal_mask, __ATOMIC_ACQUIRE);
        out->state = thread_get_state(thread);
        out->core_id = -1;
        out->tty_index = thread->tty_index;

        for (size_t i = 0; i < MAX_CORES; i++) {
            if (sched_state.cpu[i].current == thread) {
                out->core_id = (int)i;
                break;
            }
        }

        u64 cpu_ticks = __sync_fetch_and_add(&thread->cpu_time_ticks, 0);

        out->cpu_time_ms = hz ? ((cpu_ticks * 1000ULL) / hz) : 0;
        out->vm_kib = sched_user_mem_kib(thread);

        memset(out->name, 0, sizeof(out->name));
        strncpy(out->name, thread->name, sizeof(out->name) - 1);

        found = true;
    }

    sched_lock_restore(flags);

    return found;
}

void sched_cpu_usage_snapshot(u64 *busy_ticks_out, u64 *total_ticks_out) {
    if (busy_ticks_out) {
        *busy_ticks_out =
            __atomic_load_n(&sched_state.usage.busy_ticks, __ATOMIC_RELAXED);
    }

    if (total_ticks_out) {
        *total_ticks_out =
            __atomic_load_n(&sched_state.usage.total_ticks, __ATOMIC_RELAXED);
    }
}

void sched_cpu_usage_snapshot_core(
    size_t core_id,
    u64 *busy_ticks_out,
    u64 *total_ticks_out
) {
    if (core_id >= MAX_CORES) {
        if (busy_ticks_out) {
            *busy_ticks_out = 0;
        }

        if (total_ticks_out) {
            *total_ticks_out = 0;
        }

        return;
    }

    if (busy_ticks_out) {
        *busy_ticks_out =
            __atomic_load_n(&sched_state.usage.core_busy_ticks[core_id], __ATOMIC_RELAXED);
    }

    if (total_ticks_out) {
        *total_ticks_out =
            __atomic_load_n(&sched_state.usage.core_total_ticks[core_id], __ATOMIC_RELAXED);
    }
}

void sched_metrics_snapshot(sched_metrics_snapshot_t *out) {
    if (!out) {
        return;
    }

    out->sched_switch_count =
        __atomic_load_n(&sched_state.metrics.switch_count, __ATOMIC_RELAXED);
    out->sched_migrations =
        __atomic_load_n(&sched_state.metrics.migrations, __ATOMIC_RELAXED);
    out->sched_steals =
        __atomic_load_n(&sched_state.metrics.steals, __ATOMIC_RELAXED);
    out->sched_wake_ipi =
        __atomic_load_n(&sched_state.metrics.wake_ipi, __ATOMIC_RELAXED);
    out->sched_runqueue_max =
        __atomic_load_n(&sched_state.metrics.runqueue_max, __ATOMIC_RELAXED);
    out->sched_balance_runs =
        __atomic_load_n(&sched_state.metrics.balance_runs, __ATOMIC_RELAXED);
    out->wait_timeout_count =
        __atomic_load_n(&sched_state.metrics.wait_timeout_count, __ATOMIC_RELAXED);
}

int sched_getgroups_pid(
    pid_t pid,
    gid_t *primary_gid_out,
    gid_t *groups_out,
    size_t max_groups,
    size_t *group_count_out
) {
    if (pid <= 0 || !group_count_out || !sched_state.all_list) {
        return -EINVAL;
    }

    unsigned long flags = sched_lock_save();
    sched_thread_t *thread = find_thread(pid);

    if (!thread) {
        sched_lock_restore(flags);
        return -ESRCH;
    }

    if (primary_gid_out) {
        *primary_gid_out = thread->gid;
    }

    *group_count_out = thread->group_count;

    if (groups_out && max_groups) {
        size_t copy_count = thread->group_count < max_groups ? thread->group_count : max_groups;

        for (size_t i = 0; i < copy_count; i++) {
            groups_out[i] = thread->groups[i];
        }
    }

    sched_lock_restore(flags);

    return 0;
}

bool sched_proc_cwd(pid_t pid, char *out, size_t out_len) {
    if (!out || !out_len || pid <= 0 || !sched_state.all_list) {
        return false;
    }

    bool found = false;
    unsigned long flags = sched_lock_save();
    sched_thread_t *thread = find_thread(pid);

    if (thread) {
        size_t len = strnlen(thread->cwd, sizeof(thread->cwd));

        if (len + 1 <= out_len) {
            memcpy(out, thread->cwd, len);
            out[len] = '\0';
            found = true;
        }
    }

    sched_lock_restore(flags);

    return found;
}

int sched_signal_send_pgrp(pid_t pgid, int signum) {
    if (!sched_state.all_list || pgid <= 0) {
        return -1;
    }

    int count = 0;
    unsigned long flags = sched_lock_save();

    ll_foreach(node, sched_state.all_list) {
        sched_thread_t *thread = node->data;

        if (!thread || thread->pgid != pgid) {
            continue;
        }

        if (sched_signal_send_thread(thread, signum) >= 0) {
            count++;
        }
    }

    sched_lock_restore(flags);

    return count ? count : -1;
}
