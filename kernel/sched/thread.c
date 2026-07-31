#include "internal.h"

u64 _pid_index_key(pid_t pid) {
    return (u64)(u32)pid;
}

// the index can outlive the thread it names, so every hit is re-checked
// against the live list and stale entries are dropped on the way past
sched_thread_t *pid_get(pid_t pid) {
    if (!sched_state.procs.pid_index || pid <= 0) {
        return NULL;
    }

    u64 encoded = 0;
    if (!hashmap_get(sched_state.procs.pid_index, _pid_index_key(pid), &encoded)) {
        return NULL;
    }

    sched_thread_t *thread = (sched_thread_t *)(uintptr_t)encoded;
    if (thread && thread->in_all_list && thread->pid == pid) {
        return thread;
    }

    if (!hashmap_remove(sched_state.procs.pid_index, _pid_index_key(pid))) {
        panic("scheduler pid index stale-entry cleanup failed");
    }

    return NULL;
}

void pid_set(sched_thread_t *thread) {
    if (!sched_state.procs.pid_index || !thread || thread->pid <= 0) {
        return;
    }

    bool inserted = hashmap_set(sched_state.procs.pid_index, _pid_index_key(thread->pid), (u64)(uintptr_t)thread);

    if (!inserted) {
        panic("scheduler pid index insert failed");
    }
}

void pid_remove(pid_t pid) {
    if (!sched_state.procs.pid_index || pid <= 0) {
        return;
    }

    u64 encoded = 0;
    if (!hashmap_get(sched_state.procs.pid_index, _pid_index_key(pid), &encoded)) {
        return;
    }

    if (!hashmap_remove(sched_state.procs.pid_index, _pid_index_key(pid))) {
        panic("scheduler pid index remove failed");
    }
}

void thread_add(sched_thread_t *thread) {
    if (!thread || !sched_state.procs.all_list) {
        return;
    }

    unsigned long flags = sched_lock_save();

    if (thread->in_all_list) {
        sched_lock_restore(flags);
        return;
    }

    thread->all_node.data = thread;
    list_append(sched_state.procs.all_list, &thread->all_node);
    thread->in_all_list = true;
    pid_set(thread);
    sched_lock_restore(flags);
}

bool sched_fd_refs_node(const vfs_node_t *node) {
    if (!node || !sched_state.procs.all_list) {
        return false;
    }

    bool found = false;
    unsigned long flags = sched_lock_save();

    ll_foreach(entry, sched_state.procs.all_list) {
        sched_thread_t *thread = entry->data;

        if (!thread) {
            continue;
        }

        for (int fd = 0; fd < SCHED_FD_MAX; fd++) {
            if (!thread->fd_used[fd]) {
                continue;
            }

            const sched_fd_t *slot = &thread->fds[fd];
            if (!slot->file || slot->file->kind != SCHED_FD_VFS || slot->file->node != node) {
                continue;
            }

            found = true;
            break;
        }

        if (found) {
            break;
        }
    }

    sched_lock_restore(flags);
    return found;
}

void thread_set_name(sched_thread_t *thread, const char *name) {
    if (!thread) {
        return;
    }

    const char *src = name ? name : "thread";
    memset(thread->name, 0, sizeof(thread->name));

    size_t len = strnlen(src, sizeof(thread->name) - 1);

    memcpy(thread->name, src, len);
    thread->name[len] = '\0';
}

static void cleanup_thread(sched_thread_t *thread) {
    if (!thread || !sched_state.procs.all_list || !thread->in_all_list) {
        return;
    }

    pid_remove(thread->pid);
    list_remove(sched_state.procs.all_list, &thread->all_node);
    thread->in_all_list = false;
}

void thread_cleanup(sched_thread_t *thread) {
    if (!thread || !sched_state.procs.all_list) {
        return;
    }

    unsigned long flags = sched_lock_save();
    cleanup_thread(thread);
    sched_lock_restore(flags);
}

sched_thread_t *find_thread(pid_t pid) {
    if (!sched_state.procs.all_list) {
        return NULL;
    }

    sched_thread_t *thread = pid_get(pid);
    if (thread) {
        return thread;
    }

    ll_foreach(node, sched_state.procs.all_list) {
        thread = node->data;

        if (thread && thread->pid == pid) {
            pid_set(thread);
            return thread;
        }
    }

    return NULL;
}

NORETURN void thread_trampoline(void) {
    sched_thread_t *thread = sched_current();

    if (thread && thread->entry) {
        thread->entry(thread->arg);
    }

    sched_exit();
    __builtin_unreachable();
}

void thread_get(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    __atomic_fetch_add(&thread->refcount, 1, __ATOMIC_RELAXED);
}

void thread_put(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    u32 old_refs = __atomic_fetch_sub(&thread->refcount, 1, __ATOMIC_ACQ_REL);

    // an unbalanced put would wrap the count and free a live thread, so put the
    // borrowed reference back instead
    if (!old_refs) {
        __atomic_fetch_add(&thread->refcount, 1, __ATOMIC_RELAXED);
        return;
    }

    if (old_refs != 1) {
        return;
    }

    // the last reference may be dropped from the thread's own stack, so the
    // free is deferred to the reaper; the flag keeps it queued exactly once
    u32 old_flags = __atomic_fetch_or(&thread->lifecycle_flags, SCHED_DEFER_QUEUED, __ATOMIC_ACQ_REL);

    if (old_flags & SCHED_DEFER_QUEUED) {
        return;
    }

    unsigned long flags = sched_lock_save();

    if (sched_state.procs.reap_list && !thread->in_reap_list) {
        thread->reap_node.data = thread;
        list_append(sched_state.procs.reap_list, &thread->reap_node);
        thread->in_reap_list = true;
    }

    sched_lock_restore(flags);
}

void thread_destroy(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    thread->magic = 0;

    __atomic_fetch_or(&thread->lifecycle_flags, SCHED_DESTROYING, __ATOMIC_ACQ_REL);

    if (thread->pid > 0) {
        procfs_unregister_pid(thread->pid);
    }

    sched_fd_close_all(thread);

    sched_clear_user_regions(thread);

    if (thread->vm_space && thread->vm_space != sched_state.core.kernel_vm) {
        arch_vm_destroy(thread->vm_space);
    }

    sched_waitq_destroy(&thread->wait_queue);

    arch_kernel_stack_free(thread);

    free(thread);
}

static bool reapable_locked(sched_thread_t *thread) {
    if (!thread) {
        return false;
    }

    bool on_proc_list = thread->in_all_list || thread->in_zombie_list;
    bool waiting = thread->in_wait_queue || thread->sleep_queued || thread->blocked_on;

    if (on_proc_list || waiting) {
        return false;
    }

    if (thread_is_owned(thread)) {
        return false;
    }

    if (thread->on_rq || thread->in_run_queue) {
        rq_remove_thread(thread);
    }

    return !thread->on_rq && !thread->in_run_queue && thread->rq_index == UINT32_MAX;
}

static void reap_list(void) {
    if (!sched_state.procs.reap_list) {
        return;
    }

    for (;;) {
        unsigned long flags = sched_lock_save();

        list_node_t *node = list_pop_front(sched_state.procs.reap_list);
        sched_thread_t *thread = node ? node->data : NULL;

        if (thread) {
            thread->in_reap_list = false;
        }

        if (thread && !reapable_locked(thread)) {
            thread->reap_node.data = thread;
            list_append(sched_state.procs.reap_list, &thread->reap_node);
            thread->in_reap_list = true;
            sched_lock_restore(flags);
            break;
        }

        sched_lock_restore(flags);

        if (!thread) {
            break;
        }

        thread_destroy(thread);
    }
}

void sched_reap(void) {
    if (!sched_state.procs.zombie_list) {
        reap_list();
        return;
    }

    reap_list();

    unsigned long flags = sched_lock_save();
    list_node_t *node = sched_state.procs.zombie_list->head;

    while (node) {
        list_node_t *next = node->next;
        sched_thread_t *thread = node->data;

        if (thread && thread != sched_local_current() && thread != sched_local_idle()) {
            if (thread->user_thread) {
                node = next;
                continue;
            }

            if (thread_cpu(thread) >= 0) {
                node = next;
                continue;
            }

            list_remove(sched_state.procs.zombie_list, node);
            thread->in_zombie_list = false;
            cleanup_thread(thread);
            thread_put(thread);
        }

        node = next;
    }

    sched_lock_restore(flags);
}
