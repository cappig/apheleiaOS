#include "scheduler_internal.h"

u64 _pid_index_key(pid_t pid) {
    return (u64)(u32)pid;
}

sched_thread_t *_pid_index_get_locked(pid_t pid) {
    if (!sched_state.pid_index || pid <= 0) {
        return NULL;
    }

    u64 encoded = 0;
    if (!hashmap_get(sched_state.pid_index, _pid_index_key(pid), &encoded)) {
        return NULL;
    }

    sched_thread_t *thread = (sched_thread_t *)(uintptr_t)encoded;
    if (thread && thread->in_all_list && thread->pid == pid) {
        return thread;
    }

    if (!hashmap_remove(sched_state.pid_index, _pid_index_key(pid))) {
        panic("scheduler pid index stale-entry cleanup failed");
    }

    return NULL;
}

void _pid_index_set_locked(sched_thread_t *thread) {
    if (!sched_state.pid_index || !thread || thread->pid <= 0) {
        return;
    }

    if (!hashmap_set(sched_state.pid_index, _pid_index_key(thread->pid), (u64)(uintptr_t)thread)) {
        panic("scheduler pid index insert failed");
    }
}

void _pid_index_remove_locked(pid_t pid) {
    if (!sched_state.pid_index || pid <= 0) {
        return;
    }

    u64 encoded = 0;
    if (!hashmap_get(sched_state.pid_index, _pid_index_key(pid), &encoded)) {
        return;
    }

    if (!hashmap_remove(sched_state.pid_index, _pid_index_key(pid))) {
        panic("scheduler pid index remove failed");
    }
}

void add_all_thread(sched_thread_t *thread) {
    if (!thread || !sched_state.all_list) {
        return;
    }

    unsigned long flags = sched_lock_save();

    if (thread->in_all_list) {
        sched_lock_restore(flags);
        return;
    }

    thread->all_node.data = thread;
    list_append(sched_state.all_list, &thread->all_node);
    thread->in_all_list = true;
    _pid_index_set_locked(thread);
    sched_lock_restore(flags);
}

bool sched_fd_refs_node(const vfs_node_t *node) {
    if (!node || !sched_state.all_list) {
        return false;
    }

    bool found = false;
    unsigned long flags = sched_lock_save();

    ll_foreach(entry, sched_state.all_list) {
        sched_thread_t *thread = entry->data;
        if (!thread) {
            continue;
        }

        for (int fd = 0; fd < SCHED_FD_MAX; fd++) {
            if (!thread->fd_used[fd]) {
                continue;
            }

            const sched_fd_t *slot = &thread->fds[fd];
            if (slot->kind != SCHED_FD_VFS || slot->node != node) {
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

void sched_init_thread_name(sched_thread_t *thread, const char *name) {
    if (!thread) {
        return;
    }

    const char *src = name ? name : "thread";
    memset(thread->name, 0, sizeof(thread->name));

    size_t len = strnlen(src, sizeof(thread->name) - 1);

    memcpy(thread->name, src, len);
    thread->name[len] = '\0';
}

static void remove_all_thread_locked(sched_thread_t *thread) {
    if (!thread || !sched_state.all_list || !thread->in_all_list) {
        return;
    }

    _pid_index_remove_locked(thread->pid);
    list_remove(sched_state.all_list, &thread->all_node);
    thread->in_all_list = false;
}

void remove_all_thread(sched_thread_t *thread) {
    if (!thread || !sched_state.all_list) {
        return;
    }

    unsigned long flags = sched_lock_save();
    remove_all_thread_locked(thread);
    sched_lock_restore(flags);
}

sched_thread_t *find_thread_by_pid_locked(pid_t pid) {
    if (!sched_state.all_list) {
        return NULL;
    }

    sched_thread_t *thread = _pid_index_get_locked(pid);
    if (thread) {
        return thread;
    }

    ll_foreach(node, sched_state.all_list) {
        thread = node->data;

        if (thread && thread->pid == pid) {
            _pid_index_set_locked(thread);
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

void sched_thread_get(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    __atomic_fetch_add(&thread->refcount, 1, __ATOMIC_RELAXED);
}

void sched_thread_put(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    u32 prev = __atomic_fetch_sub(&thread->refcount, 1, __ATOMIC_ACQ_REL);
    if (!prev) {
        __atomic_fetch_add(&sched_state.metrics.ref_underflow, 1, __ATOMIC_RELAXED);
#if defined(DEBUG)
        panic("sched_thread_put refcount underflow");
#endif
        __atomic_fetch_add(&thread->refcount, 1, __ATOMIC_RELAXED);
        return;
    }
    if (prev != 1) {
        return;
    }

    u32 prior_lifecycle = __atomic_fetch_or(
        &thread->lifecycle_flags,
        SCHED_THREAD_LIFECYCLE_DEFER_QUEUED,
        __ATOMIC_ACQ_REL
    );
    if (prior_lifecycle & SCHED_THREAD_LIFECYCLE_DEFER_QUEUED) {
        return;
    }

    unsigned long flags = sched_lock_save();
    if (sched_state.deferred_destroy_list && !thread->in_deferred_list) {
        thread->deferred_node.data = thread;
        list_append(sched_state.deferred_destroy_list, &thread->deferred_node);
        thread->in_deferred_list = true;
    }
    sched_lock_restore(flags);
}

void destroy_thread_final(sched_thread_t *thread) {
    if (!thread) {
        return;
    }
    __atomic_fetch_or(
        &thread->lifecycle_flags,
        SCHED_THREAD_LIFECYCLE_DESTROYING,
        __ATOMIC_ACQ_REL
    );

    if (thread->pid > 0) {
        procfs_unregister_pid(thread->pid);
    }

    sched_fd_close_all(thread);

    sched_clear_user_regions(thread);

    if (thread->vm_space && thread->vm_space != sched_state.kernel_vm) {
        arch_vm_destroy(thread->vm_space);
    }

    sched_wait_queue_destroy(&thread->wait_queue);

    if (thread->stack) {
        free(thread->stack);
    }

    free(thread);
}

void sched_reap_deferred(void) {
    if (!sched_state.deferred_destroy_list) {
        return;
    }

    for (;;) {
        unsigned long flags = sched_lock_save();
        list_node_t *node = list_pop_front(sched_state.deferred_destroy_list);
        sched_thread_t *thread = node ? node->data : NULL;
        if (thread) {
            thread->in_deferred_list = false;
        }
        sched_lock_restore(flags);

        if (!thread) {
            break;
        }

        destroy_thread_final(thread);
    }
}

void sched_reap(void) {
    if (!sched_state.zombie_list) {
        sched_reap_deferred();
        return;
    }

    sched_reap_deferred();

    unsigned long flags = sched_lock_save();
    list_node_t *node = sched_state.zombie_list->head;

    while (node) {
        list_node_t *next = node->next;
        sched_thread_t *thread = node->data;

        if (thread && thread != sched_local_current() && thread != sched_local_idle()) {
            if (thread->user_thread) {
                node = next;
                continue;
            }

            list_remove(sched_state.zombie_list, node);
            thread->in_zombie_list = false;
            remove_all_thread_locked(thread);
            sched_thread_put(thread);
        }

        node = next;
    }

    sched_lock_restore(flags);
}

