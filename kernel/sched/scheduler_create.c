#include "scheduler_internal.h"

void idle_entry(UNUSED void *arg) {
    for (;;) {
        if (sched_cpu_id() == 0) {
            sched_reap();
        }
        arch_cpu_wait();
    }
}

static uintptr_t build_initial_stack(sched_thread_t *thread) {
    return arch_build_kernel_stack(thread, (uintptr_t)thread_trampoline);
}

static uintptr_t build_user_stack(
    sched_thread_t *thread,
    uintptr_t entry,
    uintptr_t user_stack_top
) {
    uintptr_t sp = (uintptr_t)thread->stack + thread->stack_size;
    sp = ALIGN_DOWN(sp, 16);

    sp -= sizeof(arch_int_state_t);
    arch_int_state_t *frame = (arch_int_state_t *)sp;
    memset(frame, 0, sizeof(*frame));

    arch_word_t user_sp = (arch_word_t)ALIGN_DOWN(user_stack_top, 16);
    arch_state_set_user_entry(frame, (arch_word_t)entry, user_sp);

    return sp;
}

static uintptr_t
build_fork_stack(sched_thread_t *thread, arch_int_state_t *state) {
    if (!thread || !state) {
        return 0;
    }

    uintptr_t sp = (uintptr_t)thread->stack + thread->stack_size;
    sp = ALIGN_DOWN(sp, 16);

    sp -= sizeof(*state);
    memcpy((void *)sp, state, sizeof(*state));

    arch_int_state_t *child_state = (arch_int_state_t *)sp;

    arch_state_set_return(child_state, 0);

    return sp;
}

void sched_prepare_user_thread(
    sched_thread_t *thread,
    uintptr_t entry,
    uintptr_t user_stack_top
) {
    if (!thread) {
        return;
    }

    thread->context = build_user_stack(thread, entry, user_stack_top);
}

bool sched_send_wake_ipi(size_t cpu_id) {
    if (cpu_id >= MAX_CORES) {
        return false;
    }

    if (!sched_mark_need_resched_cpu(cpu_id)) {
        return false;
    }

    if (cpu_id == sched_cpu_id()) {
        return false;
    }

    if (!sched_cpu_needs_wake_ipi(cpu_id)) {
        return false;
    }

    if (!smp_send_resched(cpu_id)) {
        return false;
    }

    return true;
}

typedef enum {
    SCHED_WAKE_REASON_LOCAL = 0,
    SCHED_WAKE_REASON_IDLE = 1,
    SCHED_WAKE_REASON_LOAD = 2,
} sched_wake_reason_t;

static size_t
sched_pick_target_cpu(const sched_thread_t *thread, sched_wake_reason_t *reason_out) {
    u64 online = sched_online_cpu_mask();
    u64 allowed = thread ? thread->allowed_cpu_mask : 0;
    size_t ncpu = core_count;

    if (ncpu > MAX_CORES) {
        ncpu = MAX_CORES;
    }
    if (ncpu > 64) {
        ncpu = 64;
    }
    if (!ncpu) {
        ncpu = 1;
    }

    if (!allowed) {
        allowed = online;
    }

    allowed &= online;
    if (!allowed) {
        allowed = online ? online : 1ULL;
    }

    size_t min_load = (size_t)-1;
    bool found = false;
    u64 idle_mask = 0;
    u64 min_mask = 0;

    for (size_t cpu = 0; cpu < ncpu; cpu++) {
        if (!(allowed & (1ULL << cpu))) {
            continue;
        }

        size_t load = sched_cpu_load(cpu);
        if (!load) {
            idle_mask |= (1ULL << cpu);
        }

        if (!found || load < min_load) {
            min_load = load;
            min_mask = (1ULL << cpu);
            found = true;
        } else if (load == min_load) {
            min_mask |= (1ULL << cpu);
        }
    }

    if (!found) {
        if (reason_out) {
            *reason_out = SCHED_WAKE_REASON_LOAD;
        }
        return 0;
    }

    size_t preferred_cpu = MAX_CORES;
    if (thread && thread->last_cpu < ncpu && (allowed & (1ULL << thread->last_cpu))) {
        preferred_cpu = thread->last_cpu;
    }

    if (idle_mask) {
        size_t base_cpu = preferred_cpu;
        if (base_cpu >= ncpu || !(allowed & (1ULL << base_cpu))) {
            if (min_mask) {
                for (size_t cpu = 0; cpu < ncpu; cpu++) {
                    if (min_mask & (1ULL << cpu)) {
                        base_cpu = cpu;
                        break;
                    }
                }
            } else {
                base_cpu = 0;
            }
        }

        size_t best_idle = MAX_CORES;
        size_t best_distance = (size_t)-1;
        for (size_t cpu = 0; cpu < ncpu; cpu++) {
            if (!(idle_mask & (1ULL << cpu))) {
                continue;
            }

            size_t distance = sched_cpu_distance(base_cpu, cpu, ncpu);
            if (
                best_idle >= MAX_CORES || distance < best_distance ||
                (distance == best_distance && cpu < best_idle)
            ) {
                best_idle = cpu;
                best_distance = distance;
            }
        }

        if (best_idle < MAX_CORES) {
            if (reason_out) {
                *reason_out = SCHED_WAKE_REASON_IDLE;
            }
            return best_idle;
        }
    }

    if (preferred_cpu < ncpu) {
        size_t preferred_load = sched_cpu_load(preferred_cpu);
        if (preferred_load <= min_load + SCHED_WAKE_LOCAL_LOAD_SLOP) {
            if (reason_out) {
                *reason_out = SCHED_WAKE_REASON_LOCAL;
            }
            return preferred_cpu;
        }
    }

    size_t min_cpu = 0;
    if (min_mask) {
        u32 start = __atomic_fetch_add(&sched_state.wake_rr_cursor, 1, __ATOMIC_RELAXED);
        for (size_t step = 0; step < ncpu; step++) {
            size_t cpu = (start + step) % ncpu;
            if (min_mask & (1ULL << cpu)) {
                min_cpu = cpu;
                break;
            }
        }
    }

    if (reason_out) {
        *reason_out = SCHED_WAKE_REASON_LOAD;
    }
    return min_cpu;
}

void enqueue_thread_with_ipi(sched_thread_t *thread, bool allow_remote_ipi) {
    if (!thread || thread == sched_local_idle()) {
        return;
    }

    if (!thread->context) {
        log_warn(
            "scheduler refused enqueue of contextless thread pid=%ld (%s)",
            (long)thread->pid,
            thread->name
        );
        return;
    }

    if (sched_thread_state_load(thread) != THREAD_READY) {
        return;
    }

    if (thread->pid == 0) {
        return;
    }

    sched_wake_reason_t wake_reason = SCHED_WAKE_REASON_LOAD;
    size_t target_cpu = sched_pick_target_cpu(thread, &wake_reason);
    size_t prev_cpu = thread->last_cpu;
    bool migrated = thread->on_rq && prev_cpu < MAX_CORES && prev_cpu != target_cpu;
    (void)wake_reason;

    if (migrated) {
        (void)rq_remove_thread(thread);
    }

    rq_enqueue_cpu(thread, target_cpu);

    if (prev_cpu < MAX_CORES && prev_cpu != target_cpu) {
        __atomic_fetch_add(&sched_state.metrics.migrations, 1, __ATOMIC_RELAXED);
    }

    size_t self_cpu = sched_cpu_id();
    if (target_cpu != self_cpu) {
        if (allow_remote_ipi && sched_send_wake_ipi(target_cpu)) {
            __atomic_fetch_add(&sched_state.metrics.wake_ipi, 1, __ATOMIC_RELAXED);
        } else {
            sched_set_need_resched_cpu(target_cpu, true);
        }
    } else {
        sched_request_resched_local();
    }
}

void enqueue_thread(sched_thread_t *thread) {
    enqueue_thread_with_ipi(thread, true);
}

void run_queue_remove(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    (void)rq_remove_thread(thread);
}

sched_thread_t *create_thread(
    const char *name,
    thread_entry_t entry,
    void *arg,
    bool enqueue,
    bool user_thread,
    sched_pid_class_t pid_class
) {
    sched_thread_t *thread = calloc(1, sizeof(*thread));
    if (!thread) {
        return NULL;
    }

    sched_init_thread_name(thread, name);

    thread->entry = entry;
    thread->arg = arg;
    sched_thread_state_store(thread, THREAD_READY);
    thread->affinity_core = MAX_CORES;
    thread->last_cpu = sched_cpu_id();
    thread->allowed_cpu_mask = sched_online_cpu_mask();
    thread->affinity_user_set = false;
    thread->vruntime_ns = 0;
    thread->exec_start_ns = 0;
    thread->sum_exec_ns = 0;
    thread->refcount = 1;
    thread->lifecycle_flags = 0;
    thread->user_thread = user_thread;
    thread->pid = sched_next_pid(pid_class);
    thread->ppid = 0;
    thread->rq_index = UINT32_MAX;
    sched_thread_running_cpu_store(thread, -1);

    sched_thread_t *parent = sched_local_current();

    if (user_thread) {
        if (parent && parent->user_thread && parent->pid > 0) {
            thread->pgid = parent->pgid;
            thread->sid = parent->sid;
            thread->allowed_cpu_mask = parent->allowed_cpu_mask;
            thread->affinity_user_set = parent->affinity_user_set;
            thread->vruntime_ns = parent->vruntime_ns;
        } else {
            thread->pgid = thread->pid;
            thread->sid = thread->pid;
        }
    } else {
        thread->pgid = 0;
        thread->sid = 0;
    }

    thread->uid = parent ? parent->uid : 0;
    thread->gid = parent ? parent->gid : 0;
    thread->group_count = parent ? parent->group_count : 0;
    if (parent && parent->group_count) {
        memcpy(thread->groups, parent->groups, sizeof(thread->groups));
    }
    thread->umask = parent ? parent->umask : 0022;
    thread->stack_size = SCHED_STACK_SIZE;
    thread->stack = malloc(thread->stack_size);
    thread->tty_index = parent ? parent->tty_index : -1;
    thread->sleep_queued = false;
    thread->sleep_index = 0;
    thread->wait_deadline_tick = 0;
    thread->wait_flags = 0;
    thread->wait_result = (u8)SCHED_WAIT_ABORTED;
    thread->wait_cookie = 0;

    if (!thread->stack) {
        free(thread);
        return NULL;
    }

    thread->cwd[0] = '/';
    thread->cwd[1] = '\0';

    if (!user_thread) {
        thread->context = build_initial_stack(thread);
    }

    if (user_thread) {
        thread->vm_space = arch_vm_create_user();
        if (!thread->vm_space) {
            free(thread->stack);
            free(thread);
            return NULL;
        }
    } else {
        thread->vm_space = sched_state.kernel_vm;
    }

    sched_wait_queue_init(&thread->wait_queue);
    sched_wait_queue_set_name(&thread->wait_queue, "thread_wait");
    sched_signal_init_thread(thread);

    arch_fpu_init(thread->fpu_state);
    thread->fpu_initialized = true;

    add_all_thread(thread);

    if (thread->pid > 0) {
        procfs_register_pid(thread->pid);
    }

    if (enqueue) {
        unsigned long flags = sched_lock_save();
        enqueue_thread(thread);
        sched_lock_restore(flags);
    }

    return thread;
}

sched_thread_t *sched_current(void) {
    return sched_local_current();
}

sched_thread_t *sched_current_core(size_t core_id) {
    if (core_id >= MAX_CORES) {
        return NULL;
    }

    return __atomic_load_n(&sched_state.cpu[core_id].current, __ATOMIC_ACQUIRE);
}

sched_thread_t *sched_find_thread(pid_t pid) {
    unsigned long flags = sched_lock_save();
    sched_thread_t *thread = find_thread_by_pid_locked(pid);
    if (thread && thread->in_all_list && thread->pid == pid) {
        sched_thread_get(thread);
    } else {
        thread = NULL;
    }
    sched_lock_restore(flags);
    return thread;
}

sched_thread_t *
sched_create_kernel_thread(const char *name, thread_entry_t entry, void *arg) {
    return create_thread(name, entry, arg, true, false, SCHED_PID_KERNEL);
}

sched_thread_t *sched_create_user_thread(const char *name) {
    return create_thread(name, NULL, NULL, false, true, SCHED_PID_USER);
}

pid_t sched_fork(arch_int_state_t *state) {
    sched_thread_t *parent = sched_local_current();

    if (!parent || !parent->user_thread || !state) {
        return -1;
    }

    sched_thread_t *child = sched_create_user_thread(parent->name);
    if (!child) {
        return -1;
    }

    child->ppid = parent->pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->umask = parent->umask;
    child->user_stack_base = parent->user_stack_base;
    child->user_stack_size = parent->user_stack_size;

    if (!sched_fd_clone_table(child, parent)) {
        sched_discard_thread(child);
        return -1;
    }

    memcpy(child->cwd, parent->cwd, sizeof(parent->cwd));
    memcpy(
        child->signal_handlers,
        parent->signal_handlers,
        sizeof(child->signal_handlers)
    );

    child->signal_mask = parent->signal_mask;
    child->signal_trampoline = parent->signal_trampoline;
    child->signal_pending = 0;
    child->signal_saved_valid = false;
    child->current_signal = 0;
    child->tty_index = parent->tty_index;

    if (parent->fpu_initialized) {
        memcpy(child->fpu_state, parent->fpu_state, sizeof(child->fpu_state));
        child->fpu_initialized = true;
    }

    bool cow_enabled = pmm_ref_ready();

    bool parent_tlb_needs_flush = false;
    sched_user_region_t *region = parent->regions;
    while (region) {
        size_t pages = region->pages;
        void *root = arch_vm_root(child->vm_space);

        if (!root) {
            sched_discard_thread(child);
            return -1;
        }

        if (!cow_enabled) {
            size_t size = pages * PAGE_4KIB;
            uintptr_t new_paddr = (uintptr_t)arch_alloc_frames_user(pages);

            arch_map_region(
                root, pages, region->vaddr, new_paddr, region->flags
            );
            sched_add_user_region(
                child, region->vaddr, new_paddr, pages, region->flags
            );

            void *dst = arch_phys_map(new_paddr, size, 0);
            if (!dst) {
                sched_discard_thread(child);
                return -1;
            }

            memcpy(dst, (void *)region->vaddr, size);
            arch_phys_unmap(dst, size);

            region = region->next;
            continue;
        }

        u64 region_flags = region->flags;
        bool writable = (region_flags & PT_WRITE) != 0;

        if (writable) {
            region_flags |= SCHED_REGION_COW;
        }

        region->flags = region_flags;

        u64 map_flags = region_flags;
        if (writable) {
            map_flags &= ~PT_WRITE;
        }

        arch_map_region(root, pages, region->vaddr, region->paddr, map_flags);
        sched_add_user_region(
            child, region->vaddr, region->paddr, pages, region_flags
        );

        pmm_ref_hold((void *)(uintptr_t)region->paddr, pages);

        if (writable) {
            if (sched_user_region_mark_cow(parent, region)) {
                parent_tlb_needs_flush = true;
            }
        }

        region = region->next;
    }

    if (parent_tlb_needs_flush) {
        arch_vm_switch(parent->vm_space);
    }

    child->context = build_fork_stack(child, state);

    enqueue_thread(child);
    return child->pid;
}
