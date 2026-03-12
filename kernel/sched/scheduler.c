#include "internal.h"

void scheduler_init(void) {
    if (sched_state.all_list) {
        return;
    }

    for (size_t i = 0; i < MAX_CORES; i++) {
        spinlock_init(&sched_state.runqueues[i].lock);

        sched_state.runqueues[i].capacity = SCHED_RQ_CAPACITY;
        sched_state.runqueues[i].heap =
            calloc(sched_state.runqueues[i].capacity, sizeof(sched_thread_t *));

        assert(sched_state.runqueues[i].heap);

        sched_state.runqueues[i].nr_running = 0;
        sched_state.runqueues[i].min_vruntime = 0;
    }

    sched_state.zombie_list = list_create();
    assert(sched_state.zombie_list);

    sched_state.all_list = list_create();
    assert(sched_state.all_list);

    sched_state.deferred_destroy_list = list_create();
    assert(sched_state.deferred_destroy_list);

    sched_state.pid_index = hashmap_create();

    sched_wait_queue_init(&sched_state.poll_wait_queue);
    sched_wait_queue_init(&sched_state.exit_event_wait);
    sched_wait_queue_init(&sched_state.sleep_wait_queue);

    spinlock_init(&sched_state.exit_events.lock);
    sched_state.exit_events.ring = ring_queue_create(sizeof(pid_t), SCHED_EXIT_EVENT_CAP);

    sched_state.kernel_vm = arch_vm_kernel();
    assert(sched_state.kernel_vm);

    sched_state.next_user_pid = 1;
    sched_state.next_kernel_pid = -1;
    scheduler_init_core();
    sched_running_set(false);
    sched_secondary_released_set(false);
}

void scheduler_init_core(void) {
    if (!sched_state.all_list) {
        return;
    }

    if (sched_local_idle()) {
        return;
    }

    sched_cpu_state_t *local = sched_local();
    local->handoff_ready = NULL;
    local->preempt_depth = 0;
    local->sched_lock_depth = 0;
    local->sched_lock_irq_flags = 0;
    local->slice_ns = 0;
    local->need_resched = false;
    local->force_resched = false;
    local->resched_irq_pending = false;
    local->local_ticks = 0;

    sched_thread_t *idle =
        create_thread("idle", idle_entry, NULL, false, false, SCHED_PID_IDLE);

    assert(idle);

    idle->state = THREAD_RUNNING;
    idle->affinity_core = sched_cpu_id();
    idle->last_cpu = sched_cpu_id();
    idle->running_cpu = (int)sched_cpu_id();
    idle->allowed_cpu_mask = 1ULL << (sched_cpu_id() & 63U);
    idle->tty_index = TTY_NONE;

    sched_local_set_idle(idle);
    sched_local_set_current(idle);
    sched_local_set_slice_ns(0);
    sched_local_set_need_resched(false);
    __atomic_store_n(&local->force_resched, false, __ATOMIC_RELEASE);
    __atomic_store_n(&local->resched_irq_pending, false, __ATOMIC_RELEASE);
}

static void sched_expand_default_affinity_masks(void) {
    if (!sched_state.all_list) {
        return;
    }

    u64 online = sched_online_cpu_mask();
    unsigned long flags = sched_lock_save();

    ll_foreach(node, sched_state.all_list) {
        sched_thread_t *thread = node->data;

        if (!thread || thread->pid == 0 || thread->affinity_user_set) {
            continue;
        }

        u64 mask = thread->allowed_cpu_mask;
        if (!mask) {
            mask = online;
        }

        thread->allowed_cpu_mask = mask | online;
    }

    sched_lock_restore(flags);
}

void scheduler_start(void) {
    unsigned long irq_flags = arch_irq_save();

    scheduler_init_core();

    log_info("scheduler starting");

    sched_running_set(true);
    sched_secondary_released_set(false);
    sched_expand_default_affinity_masks();
    sched_local_set_slice_ns(0);
    sched_local_set_need_resched(false);
    __atomic_store_n(&sched_local()->force_resched, false, __ATOMIC_RELEASE);
    __atomic_store_n(
        &sched_local()->resched_irq_pending, false, __ATOMIC_RELEASE
    );

    unsigned long flags = sched_lock_save();

    sched_thread_t *current = sched_local_current();
    sched_thread_t *next = pick_init_thread();

    if (!next) {
        next = pick_next_thread();
    }

    if (!current || !next || next == current) {
        sched_lock_restore(flags);
        sched_secondary_released_set(true);
        arch_irq_restore(irq_flags);
        return;
    }

    if (
        thread_ctx_ok(next) &&
        (!next->context || !ctx_valid(next))
    ) {
        sched_lock_restore(flags);
        panic("scheduler selected invalid thread context on BSP");
    }

    thread_unclaim(current);
    next->exec_start_ns = next->sum_exec_ns;
    sched_local_set_slice_ns(0);
    sched_local_set_current(next);
    thread_claim(next, sched_cpu_id());
    sched_lock_restore(flags);

    if (current->fpu_initialized) {
        arch_fpu_save(current->fpu_state);
    }

    arch_set_kernel_stack((uintptr_t)next->stack + next->stack_size);
    arch_vm_switch(next->vm_space);

    if (next->fpu_initialized) {
        arch_fpu_restore(next->fpu_state);
    }

    __atomic_fetch_add(&sched_state.metrics.switch_count, 1, __ATOMIC_RELAXED);
    sched_secondary_released_set(true);
    arch_context_switch(next->context);
    arch_irq_restore(irq_flags);
}

void scheduler_start_secondary(void) {
    unsigned long irq_flags = arch_irq_save();

    scheduler_init_core();

    if (!sched_running_get()) {
        arch_irq_restore(irq_flags);
        return;
    }

    while (!sched_secondary_released_get()) {
        if (!sched_running_get()) {
            arch_irq_restore(irq_flags);
            return;
        }

        arch_cpu_relax();
    }

    sched_local_set_slice_ns(0);
    sched_local_set_need_resched(false);
    __atomic_store_n(&sched_local()->force_resched, false, __ATOMIC_RELEASE);
    __atomic_store_n(
        &sched_local()->resched_irq_pending, false, __ATOMIC_RELEASE
    );
    sched_expand_default_affinity_masks();

    unsigned long flags = sched_lock_save();

    sched_thread_t *current = sched_local_current();
    sched_thread_t *next = pick_next_thread();

    if (!current || !next || next == current) {
        sched_lock_restore(flags);
        arch_irq_restore(irq_flags);
        return;
    }

    if (
        thread_ctx_ok(next) &&
        (!next->context || !ctx_valid(next))
    ) {
        sched_lock_restore(flags);
        panic("scheduler selected invalid thread context on AP");
    }

    thread_unclaim(current);
    next->exec_start_ns = next->sum_exec_ns;
    sched_local_set_slice_ns(0);
    sched_local_set_current(next);
    thread_claim(next, sched_cpu_id());
    sched_lock_restore(flags);

    if (current->fpu_initialized) {
        arch_fpu_save(current->fpu_state);
    }

    arch_set_kernel_stack((uintptr_t)next->stack + next->stack_size);
    arch_vm_switch(next->vm_space);

    if (next->fpu_initialized) {
        arch_fpu_restore(next->fpu_state);
    }

    __atomic_fetch_add(&sched_state.metrics.switch_count, 1, __ATOMIC_RELAXED);
    arch_context_switch(next->context);
    arch_irq_restore(irq_flags);
}

bool sched_is_running(void) {
    return sched_running_get();
}
