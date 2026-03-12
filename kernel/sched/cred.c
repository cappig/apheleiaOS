#include "internal.h"

bool sched_pid_alive(pid_t pid) {
    if (pid <= 0) {
        return false;
    }

    bool alive = false;
    unsigned long flags = sched_lock_save();
    sched_thread_t *thread = find_thread(pid);

    if (thread && thread->in_all_list && thread->pid == pid) {
        alive = (
            thread_get_state(thread) != THREAD_ZOMBIE &&
            (thread->lifecycle_flags & SCHED_THREAD_LIFECYCLE_DESTROYING) == 0
        );
    }

    sched_lock_restore(flags);
    return alive;
}

pid_t sched_getpid(void) {
    sched_thread_t *thread = sched_local_current();

    if (!thread) {
        return -EINVAL;
    }

    return thread->pid;
}

pid_t sched_getppid(void) {
    sched_thread_t *thread = sched_local_current();

    if (!thread) {
        return -EINVAL;
    }

    return thread->ppid;
}

uid_t sched_getuid(void) {
    sched_thread_t *thread = sched_local_current();

    if (!thread) {
        return (uid_t)-EINVAL;
    }

    return thread->uid;
}

gid_t sched_getgid(void) {
    sched_thread_t *thread = sched_local_current();

    if (!thread) {
        return (gid_t)-EINVAL;
    }

    return thread->gid;
}

static size_t _copy_groups(const sched_thread_t *thread, gid_t *groups, size_t max_groups) {
    if (!thread || !groups || !max_groups) {
        return 0;
    }

    size_t count = thread->group_count;
    if (count > max_groups) {
        count = max_groups;
    }

    if (count) {
        memcpy(groups, thread->groups, count * sizeof(gid_t));
    }

    return count;
}

static bool _has_group(const sched_thread_t *thread, gid_t gid) {
    if (!thread) {
        return false;
    }

    if (thread->gid == gid) {
        return true;
    }

    for (size_t i = 0; i < thread->group_count; i++) {
        if (thread->groups[i] == gid) {
            return true;
        }
    }

    return false;
}

mode_t sched_getumask(void) {
    sched_thread_t *thread = sched_local_current();

    if (!thread) {
        return (mode_t)-EINVAL;
    }

    return thread->umask & 0777;
}

int sched_setuid(uid_t uid) {
    sched_thread_t *thread = sched_local_current();
    if (!thread) {
        return -EINVAL;
    }

    if (thread->uid != 0 && uid != thread->uid) {
        return -EPERM;
    }

    thread->uid = uid;
    return 0;
}

int sched_setgid(gid_t gid) {
    sched_thread_t *thread = sched_local_current();
    if (!thread) {
        return -EINVAL;
    }

    if (thread->uid != 0 && gid != thread->gid) {
        return -EPERM;
    }

    thread->gid = gid;

    for (size_t i = 0; i < thread->group_count;) {
        if (thread->groups[i] != gid) {
            i++;
            continue;
        }

        for (size_t j = i + 1; j < thread->group_count; j++) {
            thread->groups[j - 1] = thread->groups[j];
        }

        thread->group_count--;
    }

    return 0;
}

int sched_setgroups(const gid_t *groups, size_t group_count) {
    sched_thread_t *thread = sched_local_current();
    if (!thread) {
        return -EINVAL;
    }

    if (thread->uid != 0) {
        return -EPERM;
    }

    if (group_count > SCHED_GROUP_MAX || (group_count && !groups)) {
        return -EINVAL;
    }

    size_t out_count = 0;
    for (size_t i = 0; i < group_count; i++) {
        gid_t gid = groups[i];

        if (gid == thread->gid) {
            continue;
        }

        bool seen = false;
        for (size_t j = 0; j < out_count; j++) {
            if (thread->groups[j] == gid) {
                seen = true;
                break;
            }
        }

        if (seen) {
            continue;
        }

        if (out_count >= SCHED_GROUP_MAX) {
            return -EINVAL;
        }

        thread->groups[out_count++] = gid;
    }

    thread->group_count = out_count;
    return 0;
}

int sched_getgroups(gid_t *groups, size_t max_groups, size_t *group_count_out) {
    sched_thread_t *thread = sched_local_current();
    if (!thread || !group_count_out) {
        return -EINVAL;
    }

    *group_count_out = thread->group_count;

    if (groups && max_groups) {
        _copy_groups(thread, groups, max_groups);
    }

    return 0;
}

int sched_set_affinity(pid_t pid, u64 mask) {
    sched_thread_t *self = sched_local_current();
    sched_thread_t *target = NULL;
    bool target_has_ref = false;

    u64 online = sched_online_cpu_mask();
    mask &= online;

    if (!mask) {
        return -EINVAL;
    }

    if (pid == 0) {
        target = self;
    } else if (pid > 0) {
        target = sched_find_thread(pid);
        target_has_ref = true;
    } else {
        return -EINVAL;
    }

    if (!target) {
        return -ESRCH;
    }

    if (self && self->uid != 0 && self->uid != target->uid) {
        if (target_has_ref) {
            thread_put(target);
        }

        return -EPERM;
    }

    unsigned long flags = sched_lock_save();

    target->allowed_cpu_mask = mask;
    target->affinity_user_set = true;

    if (target->on_rq && target->state == THREAD_READY) {
        rq_remove(target);
        enqueue_thread(target);
    }

    bool request_local_resched = false;
    size_t request_remote_resched_cpu = MAX_CORES;

    if (target->state == THREAD_RUNNING && !sched_cpu_allowed(target, target->last_cpu)) {
        size_t cpu = target->last_cpu;

        if (cpu < MAX_CORES) {
            if (cpu == sched_cpu_id()) {
                request_local_resched = true;
            } else {
                request_remote_resched_cpu = cpu;
            }
        }
    }

    sched_lock_restore(flags);

    if (request_local_resched) {
        sched_request_resched_local();
    } else if (
        request_remote_resched_cpu < MAX_CORES &&
        wake_cpu(request_remote_resched_cpu)
    ) {
        __atomic_fetch_add(&sched_state.metrics.wake_ipi, 1, __ATOMIC_RELAXED);
    }

    if (target_has_ref) {
        thread_put(target);
    }

    return 0;
}

int sched_get_affinity(pid_t pid, u64 *mask_out) {
    if (!mask_out) {
        return -EINVAL;
    }

    sched_thread_t *self = sched_local_current();
    sched_thread_t *target = NULL;
    bool target_has_ref = false;

    if (pid == 0) {
        target = self;
    } else if (pid > 0) {
        target = sched_find_thread(pid);
        target_has_ref = true;
    } else {
        return -EINVAL;
    }

    if (!target) {
        return -ESRCH;
    }

    if (self && self->uid != 0 && self->uid != target->uid) {
        if (target_has_ref) {
            thread_put(target);
        }

        return -EPERM;
    }

    unsigned long flags = sched_lock_save();

    u64 mask = target->allowed_cpu_mask;
    if (!mask) {
        mask = sched_online_cpu_mask();
    }

    sched_lock_restore(flags);

    *mask_out = mask;

    if (target_has_ref) {
        thread_put(target);
    }

    return 0;
}

bool sched_gid_matches_cred(uid_t uid, gid_t gid, gid_t target_gid) {
    if (gid == target_gid) {
        return true;
    }

    sched_thread_t *thread = sched_local_current();
    if (!thread) {
        return false;
    }

    if (thread->uid != uid || thread->gid != gid) {
        return false;
    }

    return _has_group(thread, target_gid);
}

int sched_setumask(mode_t mask) {
    sched_thread_t *thread = sched_local_current();
    if (!thread) {
        return -EINVAL;
    }

    thread->umask = mask & 0777;

    return 0;
}

pid_t sched_getpgid(pid_t pid) {
    sched_thread_t *thread = sched_local_current();
    if (!thread) {
        return -EINVAL;
    }

    if (!pid) {
        return thread->pgid;
    }

    unsigned long flags = sched_lock_save();

    sched_thread_t *target = find_thread(pid);
    pid_t pgid = target ? target->pgid : (pid_t)-ESRCH;

    sched_lock_restore(flags);

    return pgid;
}

int sched_setpgid(pid_t pid, pid_t pgid) {
    sched_thread_t *self = sched_local_current();

    if (!self || !self->user_thread) {
        return -EINVAL;
    }

    if (pid < 0 || pgid < 0) {
        return -EINVAL;
    }

    unsigned long flags = sched_lock_save();

    sched_thread_t *target = self;
    if (pid > 0) {
        target = find_thread(pid);
    }

    if (!target || !target->user_thread) {
        sched_lock_restore(flags);
        return -ESRCH;
    }

    if (target->pid != self->pid && target->ppid != self->pid) {
        sched_lock_restore(flags);
        return -ESRCH;
    }

    if (target->sid != self->sid) {
        sched_lock_restore(flags);
        return -EPERM;
    }

    if (target->sid == target->pid) {
        sched_lock_restore(flags);
        return -EPERM;
    }

    if (!pgid) {
        pgid = target->pid;
    }

    if (pgid != target->pid) {
        bool found = false;

        ll_foreach(node, sched_state.all_list) {
            sched_thread_t *iter = node->data;

            if (!iter) {
                continue;
            }

            if (iter->pgid == pgid && iter->sid == self->sid) {
                found = true;
                break;
            }
        }

        if (!found) {
            sched_lock_restore(flags);
            return -EPERM;
        }
    }

    target->pgid = pgid;
    sched_lock_restore(flags);

    return 0;
}

pid_t sched_setsid(void) {
    sched_thread_t *thread = sched_local_current();

    if (!thread || !thread->user_thread) {
        return -EINVAL;
    }

    unsigned long flags = sched_lock_save();
    bool pgrp_exists = false;

    if (sched_state.all_list) {
        ll_foreach(node, sched_state.all_list) {
            sched_thread_t *iter = node->data;

            if (!iter) {
                continue;
            }

            if (iter->pgid == thread->pid) {
                pgrp_exists = true;
                break;
            }
        }
    }

    if (pgrp_exists) {
        sched_lock_restore(flags);
        return -EPERM;
    }

    thread->sid = thread->pid;
    thread->pgid = thread->pid;
    thread->tty_index = TTY_NONE;

    sched_lock_restore(flags);

    return thread->sid;
}

bool sched_process_is_child(pid_t child_pid, pid_t parent_pid) {
    if (child_pid <= 0 || parent_pid <= 0) {
        return false;
    }

    unsigned long flags = sched_lock_save();

    sched_thread_t *thread = find_thread(child_pid);
    bool is_child = thread && thread->ppid == parent_pid;

    sched_lock_restore(flags);

    return is_child;
}

bool sched_pid_is_group_leader(pid_t pid) {
    if (pid <= 0) {
        return false;
    }

    unsigned long flags = sched_lock_save();

    sched_thread_t *thread = find_thread(pid);
    bool is_leader = thread && thread->pgid == pid;

    sched_lock_restore(flags);

    return is_leader;
}

bool sched_pgrp_exists(pid_t pgid) {
    if (pgid <= 0 || !sched_state.all_list) {
        return false;
    }

    bool found = false;
    unsigned long flags = sched_lock_save();

    ll_foreach(node, sched_state.all_list) {
        sched_thread_t *thread = node->data;

        if (!thread) {
            continue;
        }

        if (thread->pgid != pgid) {
            continue;
        }

        found = true;
        break;
    }

    sched_lock_restore(flags);
    return found;
}

bool sched_pgrp_in_session(pid_t pgid, pid_t sid) {
    if (pgid <= 0 || sid <= 0 || !sched_state.all_list) {
        return false;
    }

    bool found = false;
    unsigned long flags = sched_lock_save();

    ll_foreach(node, sched_state.all_list) {
        sched_thread_t *thread = node->data;

        if (!thread) {
            continue;
        }

        if (thread->pgid != pgid || thread->sid != sid) {
            continue;
        }

        found = true;
        break;
    }

    sched_lock_restore(flags);
    return found;
}
