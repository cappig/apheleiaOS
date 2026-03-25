#include "internal.h"

#include <inttypes.h>

u64 _pid_index_key(pid_t pid) {
    return (u64)(u32)pid;
}

static sched_thread_t *_pid_get_locked(pid_t pid) {
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

static void _pid_set_locked(sched_thread_t *thread) {
    if (!sched_state.pid_index || !thread || thread->pid <= 0) {
        return;
    }

    if (!hashmap_set(sched_state.pid_index, _pid_index_key(thread->pid), (u64)(uintptr_t)thread)) {
        panic("scheduler pid index insert failed");
    }
}

static void _pid_remove_locked(pid_t pid) {
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

static int sched_thread_current_cpu_locked(const sched_thread_t *thread) {
    if (!thread) {
        return -1;
    }

    for (size_t cpu_id = 0; cpu_id < MAX_CORES; cpu_id++) {
        sched_thread_t *current = __atomic_load_n(
            &sched_state.cpu[cpu_id].current,
            __ATOMIC_ACQUIRE
        );
        if (current == thread) {
            return (int)cpu_id;
        }
    }

    return -1;
}

static int sched_thread_handoff_cpu_locked(const sched_thread_t *thread) {
    if (!thread) {
        return -1;
    }

    for (size_t cpu_id = 0; cpu_id < MAX_CORES; cpu_id++) {
        sched_thread_t *handoff = __atomic_load_n(
            &sched_state.cpu[cpu_id].handoff_ready,
            __ATOMIC_ACQUIRE
        );
        if (handoff == thread) {
            return (int)cpu_id;
        }
    }

    return -1;
}

static size_t sched_thread_rq_hits_locked(
    const sched_thread_t *thread,
    int *first_cpu_out
) {
    if (first_cpu_out) {
        *first_cpu_out = -1;
    }

    if (!thread) {
        return 0;
    }

    size_t hits = 0;

    for (size_t cpu_id = 0; cpu_id < MAX_CORES; cpu_id++) {
        sched_rq_t *rq = &sched_state.runqueues[cpu_id];
        if (!rq->heap) {
            continue;
        }

        for (size_t i = 0; i < rq->nr_running; i++) {
            if (rq->heap[i] != thread) {
                continue;
            }

            if (first_cpu_out && *first_cpu_out < 0) {
                *first_cpu_out = (int)cpu_id;
            }

            hits++;
        }
    }

    return hits;
}

sched_thread_t *pid_get(pid_t pid) {
    unsigned long flags = sched_lock_save();
    sched_thread_t *thread = _pid_get_locked(pid);
    sched_lock_restore(flags);
    return thread;
}

void pid_set(sched_thread_t *thread) {
    unsigned long flags = sched_lock_save();
    _pid_set_locked(thread);
    sched_lock_restore(flags);
}

void pid_remove(pid_t pid) {
    unsigned long flags = sched_lock_save();
    _pid_remove_locked(pid);
    sched_lock_restore(flags);
}

void thread_add(sched_thread_t *thread) {
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
    _pid_set_locked(thread);
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

void sched_cull_invalid_thread_locked(
    sched_thread_t *thread,
    const char *site
) {
    if (!thread || sched_thread_is_idle(thread)) {
        return;
    }

    const arch_int_state_t *state = (const arch_int_state_t *)thread->context;
    bool frame_ok = ctx_candidate_valid(thread, state);
    bool user_frame = frame_ok && arch_signal_is_user(state);
    int current_cpu = sched_thread_current_cpu_locked(thread);
    int handoff_cpu = sched_thread_handoff_cpu_locked(thread);
    int rq_first_cpu = -1;
    size_t rq_hits = sched_thread_rq_hits_locked(thread, &rq_first_cpu);
    int running_cpu = thread_cpu(thread);
    const u32 *ctx_words = (const u32 *)state;
    size_t ctx_word_count = sizeof(*state) / sizeof(u32);
    size_t tail_index = ctx_word_count > 6 ? (ctx_word_count - 6) : 0;

    if (frame_ok) {
        u64 sp = user_frame ? (u64)arch_state_sp(state) : 0;
        u64 ss = user_frame ? (u64)state->s_regs.ss : 0;
        log_warn(
            "sched invalid context site=%s thread=%#" PRIx64 " pid=%d ppid=%d"
            " name=%s ctx=%#" PRIx64 " ip=%#" PRIx64 " sp=%#" PRIx64
            " cs=%#" PRIx64 " ss=%#" PRIx64 " user=%d"
            " state=%d running_cpu=%d current_cpu=%d handoff_cpu=%d"
            " on_rq=%d rq_index=%u rq_hits=%zu rq_first_cpu=%d"
            " in_wait=%d sleepq=%d",
            site ? site : "?",
            (u64)(uintptr_t)thread,
            thread->pid,
            thread->ppid,
            thread->name,
            (u64)(uintptr_t)thread->context,
            (u64)arch_state_ip(state),
            sp,
            (u64)state->s_regs.cs,
            ss,
            user_frame ? 1 : 0,
            thread_get_state(thread),
            running_cpu,
            current_cpu,
            handoff_cpu,
            thread->on_rq ? 1 : 0,
            thread->rq_index,
            rq_hits,
            rq_first_cpu,
            thread->in_wait_queue ? 1 : 0,
            thread->sleep_queued ? 1 : 0
        );
        if (ctx_word_count >= 6) {
            log_warn(
                "sched invalid ctx head site=%s w0=%#x w1=%#x w2=%#x"
                " w3=%#x w4=%#x w5=%#x",
                site ? site : "?",
                ctx_words[0],
                ctx_words[1],
                ctx_words[2],
                ctx_words[3],
                ctx_words[4],
                ctx_words[5]
            );
        }
        if (ctx_word_count >= tail_index + 6) {
            log_warn(
                "sched invalid ctx tail site=%s wt0=%#x wt1=%#x wt2=%#x"
                " wt3=%#x wt4=%#x wt5=%#x ctx_words=%zu stack=%#" PRIx64
                "-%#" PRIx64,
                site ? site : "?",
                ctx_words[tail_index + 0],
                ctx_words[tail_index + 1],
                ctx_words[tail_index + 2],
                ctx_words[tail_index + 3],
                ctx_words[tail_index + 4],
                ctx_words[tail_index + 5],
                ctx_word_count,
                (u64)(uintptr_t)thread->stack,
                (u64)((uintptr_t)thread->stack + thread->stack_size)
            );
        }
    } else {
        log_warn(
            "sched invalid context site=%s thread=%#" PRIx64 " pid=%d ppid=%d"
            " name=%s ctx=%#" PRIx64 " stack=%#" PRIx64 " size=%zu"
            " state=%d running_cpu=%d current_cpu=%d handoff_cpu=%d"
            " on_rq=%d rq_index=%u rq_hits=%zu rq_first_cpu=%d"
            " in_wait=%d sleepq=%d",
            site ? site : "?",
            (u64)(uintptr_t)thread,
            thread->pid,
            thread->ppid,
            thread->name,
            (u64)(uintptr_t)thread->context,
            (u64)(uintptr_t)thread->stack,
            thread->stack_size,
            thread_get_state(thread),
            running_cpu,
            current_cpu,
            handoff_cpu,
            thread->on_rq ? 1 : 0,
            thread->rq_index,
            rq_hits,
            rq_first_cpu,
            thread->in_wait_queue ? 1 : 0,
            thread->sleep_queued ? 1 : 0
        );
        if (ctx_word_count >= 6) {
            log_warn(
                "sched invalid raw head site=%s w0=%#x w1=%#x w2=%#x"
                " w3=%#x w4=%#x w5=%#x ctx_words=%zu",
                site ? site : "?",
                ctx_words[0],
                ctx_words[1],
                ctx_words[2],
                ctx_words[3],
                ctx_words[4],
                ctx_words[5],
                ctx_word_count
            );
        }
    }

    thread_unclaim(thread);
    thread_set_state(thread, THREAD_ZOMBIE);
    thread->exit_code = -EFAULT;

    if (sched_state.zombie_list && !thread->in_zombie_list) {
        thread->zombie_node.data = thread;
        list_append(sched_state.zombie_list, &thread->zombie_node);
        thread->in_zombie_list = true;
    }

    if (thread->user_thread) {
        sched_thread_t *parent = _pid_get_locked(thread->ppid);
        if (parent) {
            sched_wake_one_locked(&parent->wait_queue);
        }
    }

    if (thread->pid > 0) {
        exit_event_push(thread->pid);
    }
}

static void cleanup_thread(sched_thread_t *thread) {
    if (!thread || !sched_state.all_list || !thread->in_all_list) {
        return;
    }

    _pid_remove_locked(thread->pid);
    list_remove(sched_state.all_list, &thread->all_node);
    thread->in_all_list = false;
}

void thread_cleanup(sched_thread_t *thread) {
    if (!thread || !sched_state.all_list) {
        return;
    }

    unsigned long flags = sched_lock_save();
    rq_remove_thread(thread);
    wq_dequeue(thread);
    sleep_heap_remove(thread);
    thread_unclaim(thread);
    cleanup_thread(thread);
    sched_lock_restore(flags);
}

sched_thread_t *find_thread(pid_t pid) {
    unsigned long flags = sched_lock_save();
    sched_thread_t *thread = NULL;

    if (sched_state.all_list) {
        thread = _pid_get_locked(pid);
        if (!thread) {
            ll_foreach(node, sched_state.all_list) {
                thread = node->data;

                if (thread && thread->pid == pid) {
                    _pid_set_locked(thread);
                    break;
                }
            }
        }
    }

    sched_lock_restore(flags);
    return thread;
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

    u32 prev = __atomic_fetch_sub(&thread->refcount, 1, __ATOMIC_ACQ_REL);
    if (!prev) {
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

void sched_release_retired_current_cpu(void) {
    if (!sched_running_get()) {
        return;
    }

    sched_thread_t *retired = NULL;
    unsigned long flags = sched_lock_save();

    retired = sched_local()->retired_thread;
    sched_local()->retired_thread = NULL;

    sched_lock_restore(flags);

    if (retired) {
        thread_put(retired);
    }
}

void thread_destroy(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    thread->magic = 0;

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

    arch_kernel_stack_free(thread);

    free(thread);
}

static bool thread_has_active_cpu_slot_locked(const sched_thread_t *thread) {
    return sched_thread_has_active_cpu_slot(thread);
}

static bool thread_destroy_ready_locked(sched_thread_t *thread) {
    if (!thread) {
        return false;
    }

    if (
        thread->in_all_list || thread->in_zombie_list || thread->in_wait_queue ||
        thread->sleep_queued || thread->blocked_on
    ) {
        return false;
    }

    if (thread_has_active_cpu_slot_locked(thread)) {
        return false;
    }

    rq_purge_thread(thread);

    if (thread->on_rq || thread->in_run_queue || thread->rq_index != UINT32_MAX) {
        return false;
    }

    return !thread_has_active_cpu_slot_locked(thread);
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

        if (!thread) {
            sched_lock_restore(flags);
            break;
        }

        if (!thread_destroy_ready_locked(thread)) {
            if (!thread->in_deferred_list) {
                thread->deferred_node.data = thread;
                list_append(sched_state.deferred_destroy_list, &thread->deferred_node);
                thread->in_deferred_list = true;
            }
            sched_lock_restore(flags);
            break;
        }

        sched_lock_restore(flags);
        thread_destroy(thread);
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

            if (sched_thread_has_active_cpu_slot(thread)) {
                node = next;
                continue;
            }

            list_remove(sched_state.zombie_list, node);
            thread->in_zombie_list = false;
            cleanup_thread(thread);
            thread_put(thread);
        }

        node = next;
    }

    sched_lock_restore(flags);
}
