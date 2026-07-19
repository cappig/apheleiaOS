#include "internal.h"

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

static uintptr_t build_user_stack(sched_thread_t *thread, uintptr_t entry, uintptr_t user_stack_top) {
    uintptr_t sp = (uintptr_t)thread->stack + thread->stack_size;
    sp = ALIGN_DOWN(sp, 16);

    sp -= sizeof(arch_int_state_t);
    arch_int_state_t *frame = (arch_int_state_t *)sp;
    memset(frame, 0, sizeof(*frame));

    arch_word_t user_sp = (arch_word_t)ALIGN_DOWN(user_stack_top, 16);
    arch_set_user_entry(frame, (arch_word_t)entry, user_sp);

    return sp;
}

static uintptr_t build_fork_stack(sched_thread_t *thread, arch_int_state_t *state) {
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

static pid_t _fork_fail(const char *reason, int error) {
    if (reason && reason[0]) {
        sched_thread_t *thread = sched_local_current();
        log_warn(
            "fork failed for pid=%ld (%s): %s",
            thread ? (long)thread->pid : 0L,
            thread ? thread->name : "unknown",
            reason
        );
    }

    return error > 0 ? -error : -ENOMEM;
}

void thread_prepare_user(sched_thread_t *thread, uintptr_t entry, uintptr_t user_stack_top) {
    if (!thread) {
        return;
    }

    thread->context = build_user_stack(thread, entry, user_stack_top);
}

bool wake_cpu(size_t cpu_id) {
    if (cpu_id >= MAX_CORES) {
        return false;
    }

    if (!sched_mark_resched_cpu(cpu_id)) {
        return false;
    }

    if (cpu_id == sched_cpu_id()) {
        return false;
    }

    if (!cpu_needs_ipi(cpu_id)) {
        return false;
    }

    if (!arch_resched_cpu(cpu_id)) {
        return false;
    }

    return true;
}

static size_t wake_cpu_count(void) {
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

    return ncpu;
}

static u64 wake_mask(const sched_thread_t *thread, u64 online) {
    u64 allowed = thread ? thread->allowed_cpu_mask : 0;

    if (!allowed) {
        allowed = online;
    }

    allowed &= online;
    if (!allowed) {
        allowed = online ? online : 1ULL;
    }

    return allowed;
}

static size_t first_cpu_in_mask(u64 mask, size_t ncpu) {
    for (size_t cpu = 0; cpu < ncpu; cpu++) {
        if (mask & (1ULL << cpu)) {
            return cpu;
        }
    }

    return 0;
}

static size_t pick_idle_cpu(u64 idle_mask, size_t base_cpu, size_t ncpu) {
    size_t best_cpu = MAX_CORES;
    size_t best_distance = (size_t)-1;

    for (size_t cpu = 0; cpu < ncpu; cpu++) {
        if (!(idle_mask & (1ULL << cpu))) {
            continue;
        }

        size_t distance = sched_cpu_distance(base_cpu, cpu, ncpu);
        bool no_best = best_cpu >= MAX_CORES;
        bool closer = distance < best_distance;
        bool tie_break = distance == best_distance && cpu < best_cpu;
        bool better = no_best || closer || tie_break;

        if (better) {
            best_cpu = cpu;
            best_distance = distance;
        }
    }

    return best_cpu;
}

static size_t pick_min_cpu(u64 min_mask, size_t ncpu) {
    u32 start = __atomic_fetch_add(&sched_state.cpus.wake_rr_cursor, 1, __ATOMIC_RELAXED);

    for (size_t step = 0; step < ncpu; step++) {
        size_t cpu = (start + step) % ncpu;

        if (min_mask & (1ULL << cpu)) {
            return cpu;
        }
    }

    return 0;
}

static bool wake_can_prefer(const sched_thread_t *thread, size_t ncpu, u64 allowed) {
    if (!thread) {
        return false;
    }

    if (thread->last_cpu >= ncpu) {
        return false;
    }

    return allowed & (1ULL << thread->last_cpu);
}

static size_t pick_target_cpu(const sched_thread_t *thread) {
    size_t ncpu = wake_cpu_count();
    u64 online = sched_online_cpu_mask();
    u64 allowed = wake_mask(thread, online);

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
        return 0;
    }

    size_t preferred_cpu = MAX_CORES;

    if (wake_can_prefer(thread, ncpu, allowed)) {
        preferred_cpu = thread->last_cpu;
    }

    if (idle_mask) {
        size_t base_cpu = preferred_cpu;

        if (base_cpu >= ncpu || !(allowed & (1ULL << base_cpu))) {
            base_cpu = first_cpu_in_mask(min_mask, ncpu);
        }

        size_t best_idle = pick_idle_cpu(idle_mask, base_cpu, ncpu);
        if (best_idle < MAX_CORES) {
            return best_idle;
        }
    }

    if (preferred_cpu < ncpu) {
        size_t preferred_load = sched_cpu_load(preferred_cpu);

        if (preferred_load <= min_load + SCHED_WAKE_LOAD_SLOP) {
            return preferred_cpu;
        }
    }

    return min_mask ? pick_min_cpu(min_mask, ncpu) : 0;
}

void enqueue_ipi(sched_thread_t *thread, bool allow_remote_ipi) {
    if (!thread || thread == sched_local_idle()) {
        return;
    }

    if (!thread->context) {
        return;
    }

    if (thread_get_state(thread) != THREAD_READY) {
        return;
    }

    if (thread->pid == 0) {
        return;
    }

    size_t target_cpu = pick_target_cpu(thread);
    size_t prev_cpu = thread->last_cpu;

    if (thread->on_rq && prev_cpu != target_cpu) {
        rq_remove_thread(thread);
    }

    rq_enqueue_cpu(thread, target_cpu);

    if (prev_cpu != target_cpu) {
        __atomic_fetch_add(&sched_state.metrics.migrations, 1, __ATOMIC_RELAXED);
    }

    size_t self_cpu = sched_cpu_id();

    if (target_cpu != self_cpu) {
        if (allow_remote_ipi && wake_cpu(target_cpu)) {
            __atomic_fetch_add(&sched_state.metrics.wake_ipi, 1, __ATOMIC_RELAXED);
        } else {
            sched_set_resched_cpu(target_cpu, true);
        }
    } else {
        sched_resched_local();
    }
}

void enqueue_thread(sched_thread_t *thread) {
    enqueue_ipi(thread, true);
}

void rq_remove(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    rq_remove_thread(thread);
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
        log_warn("failed to allocate scheduler thread object");
        return NULL;
    }

    thread_set_name(thread, name);

    thread->entry = entry;
    thread->arg = arg;
    thread_set_state(thread, THREAD_READY);
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
    thread_set_cpu(thread, -1);

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

    thread->ruid = parent ? parent->ruid : 0;
    thread->uid = parent ? parent->uid : 0;
    thread->suid = parent ? parent->suid : 0;
    thread->rgid = parent ? parent->rgid : 0;
    thread->gid = parent ? parent->gid : 0;
    thread->sgid = parent ? parent->sgid : 0;
    thread->group_count = parent ? parent->group_count : 0;
    if (parent && parent->group_count) {
        memcpy(thread->groups, parent->groups, sizeof(thread->groups));
    }
    thread->umask = parent ? parent->umask : 0022;
    thread->stack_size = SCHED_STACK_SIZE;
    thread->tty_index = parent ? parent->tty_index : -1;
    thread->sleep_queued = false;
    thread->sleep_index = 0;
    thread->wait_deadline_tick = 0;
    thread->wait_flags = 0;
    thread->wait_result = (u8)SCHED_WAIT_ABORTED;
    thread->wait_cookie = 0;

    if (!arch_kernel_stack_alloc(thread)) {
        log_warn("failed to allocate scheduler thread stack");
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
            log_warn("failed to allocate user VM space");
            arch_kernel_stack_free(thread);
            free(thread);
            return NULL;
        }
    } else {
        thread->vm_space = sched_state.core.kernel_vm;
    }

    spinlock_init(&thread->vm_lock);

    sched_waitq_init(&thread->wait_queue);
    sched_signal_init(thread);

    arch_fpu_init(thread->fpu_state);
    thread->fpu_initialized = true;

    thread_add(thread);

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

    return __atomic_load_n(&sched_state.cpus.cpu[core_id].current, __ATOMIC_ACQUIRE);
}

sched_thread_t *sched_find_thread(pid_t pid) {
    unsigned long flags = sched_lock_save();
    sched_thread_t *thread = find_thread(pid);

    if (thread && thread->in_all_list && thread->pid == pid) {
        thread_get(thread);
    } else {
        thread = NULL;
    }

    sched_lock_restore(flags);

    return thread;
}

sched_thread_t *sched_spawn_kernel(const char *name, thread_entry_t entry, void *arg) {
    return create_thread(name, entry, arg, true, false, SCHED_PID_KERNEL);
}

sched_thread_t *sched_create_user_thread(const char *name) {
    return create_thread(name, NULL, NULL, false, true, SCHED_PID_USER);
}

static void copy_fork_state(sched_thread_t *child, sched_thread_t *parent) {
    child->ppid = parent->pid;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->umask = parent->umask;
    child->user_stack_base = parent->user_stack_base;
    child->user_stack_size = parent->user_stack_size;

    memcpy(child->cwd, parent->cwd, sizeof(parent->cwd));
    memcpy(child->signal_handlers, parent->signal_handlers, sizeof(child->signal_handlers));

    child->signal_mask = parent->signal_mask;
    child->signal_trampoline = parent->signal_trampoline;
    child->signal_pending = 0;
    __atomic_store_n(&child->signal_saved_valid, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&child->current_signal, 0, __ATOMIC_RELEASE);
    child->tty_index = parent->tty_index;

    if (parent->fpu_initialized) {
        memcpy(child->fpu_state, parent->fpu_state, sizeof(child->fpu_state));
        child->fpu_initialized = true;
    }
}

static bool region_bytes(const sched_user_region_t *region, size_t *size_out) {
    size_t pages = region->pages;
    bool size_overflows = pages > (uintptr_t)-1 / PAGE_4KIB;
    size_t size = pages * PAGE_4KIB;
    bool end_overflows = !size_overflows && region->vaddr > (uintptr_t)-1 - size;

    if (size_overflows || end_overflows) {
        return false;
    }

    *size_out = size;
    return true;
}

static void unmap_child_region(void *root, uintptr_t vaddr, size_t pages) {
    for (size_t i = 0; i < pages; i++) {
        uintptr_t page = vaddr + i * PAGE_4KIB;

        unmap_page((page_t *)root, page);
        arch_tlb_flush(page);
    }
}

static int fork_copy_region(sched_thread_t *child, const sched_user_region_t *region, void *root) {
    size_t size = 0;
    if (!region_bytes(region, &size)) {
        return -ENOMEM;
    }

    uintptr_t new_paddr = (uintptr_t)arch_alloc_frames_user(region->pages);
    if (!new_paddr) {
        return -ENOMEM;
    }

    arch_map_region(root, region->pages, region->vaddr, new_paddr, region->flags);

    if (!sched_add_user_region(child, region->vaddr, new_paddr, region->pages, region->flags)) {
        unmap_child_region(root, region->vaddr, region->pages);
        arch_free_frames((void *)new_paddr, region->pages);
        return -ENOMEM;
    }

    void *dst = arch_phys_map(new_paddr, size, 0);
    if (!dst) {
        return -ENOMEM;
    }

    memcpy(dst, (void *)region->vaddr, size);
    arch_phys_unmap(dst, size);
    return 0;
}

static int fork_cow_region(
    sched_thread_t *parent,
    sched_thread_t *child,
    sched_user_region_t *region,
    void *root,
    bool *flush_parent_tlb
) {
    u64 flags = region->flags;
    bool writable = (flags & PT_WRITE) != 0;

    if (writable) {
        flags |= SCHED_REGION_COW;
    }

    region->flags = flags;

    u64 map_flags = writable ? (flags & ~PT_WRITE) : flags;

    arch_map_region(root, region->pages, region->vaddr, region->paddr, map_flags);
    if (!sched_add_user_region(child, region->vaddr, region->paddr, region->pages, flags)) {
        return -ENOMEM;
    }

    pmm_ref_hold((void *)(uintptr_t)region->paddr, region->pages);

    if (writable && sched_mark_cow(parent, region)) {
        *flush_parent_tlb = true;
    }

    return 0;
}

static int fork_user_space(sched_thread_t *parent, sched_thread_t *child, bool *flush_parent_tlb) {
    void *root = arch_vm_root(child->vm_space);
    if (!root) {
        return -ENOMEM;
    }

    bool cow_enabled = pmm_ref_ready();

    for (sched_user_region_t *region = parent->regions; region; region = region->next) {
        int status = cow_enabled ? fork_cow_region(parent, child, region, root, flush_parent_tlb)
                                 : fork_copy_region(child, region, root);
        if (status < 0) {
            return status;
        }
    }

    return 0;
}

static int fork_clone_vm(sched_thread_t *parent, sched_thread_t *child) {
    bool flush_parent_tlb = false;
    unsigned long flags = spin_lock_irqsave(&parent->vm_lock);

    int status = fork_user_space(parent, child, &flush_parent_tlb);

    if (status == 0 && flush_parent_tlb) {
        arch_vm_switch(parent->vm_space);
    }

    spin_unlock_irqrestore(&parent->vm_lock, flags);
    return status;
}

static void fork_make_runnable(sched_thread_t *child, arch_int_state_t *state) {
    child->context = build_fork_stack(child, state);
    child->vruntime_ns = 0;

    unsigned long flags = sched_lock_save();
    enqueue_thread(child);
    sched_lock_restore(flags);
}

pid_t sched_fork(arch_int_state_t *state) {
    sched_thread_t *parent = sched_local_current();

    if (!parent || !parent->user_thread || !state) {
        return _fork_fail("invalid parent or missing trap state", EINVAL);
    }

    sched_thread_t *child = sched_create_user_thread(parent->name);
    if (!child) {
        return _fork_fail("failed to create child thread", ENOMEM);
    }

    copy_fork_state(child, parent);

    if (!sched_fd_clone_table(child, parent)) {
        sched_discard_thread(child);
        return _fork_fail("failed to clone file descriptor table", ENOMEM);
    }

    if (fork_clone_vm(parent, child) < 0) {
        sched_discard_thread(child);
        return _fork_fail("failed to clone user address space", ENOMEM);
    }

    fork_make_runnable(child, state);
    return child->pid;
}
