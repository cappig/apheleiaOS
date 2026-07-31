#include "internal.h"

// the idle thread yields the moment anything is runnable; a real thread keeps
// the cpu until its slice is spent or a thread with less runtime shows up
static bool should_preempt(sched_thread_t *thread, size_t cpu_id) {
    size_t rq_depth = sched_rq_depth(cpu_id);

    if (thread == sched_local_idle() && thread_get_state(thread) != THREAD_RUNNING) {
        thread_claim(thread, cpu_id);
    }

    if (thread_get_state(thread) != THREAD_RUNNING) {
        return true;
    }

    if (thread == sched_local_idle()) {
        return rq_depth != 0;
    }

    if (!sched_cpu_allowed(thread, cpu_id)) {
        return true;
    }

    u64 target_ns = sched_target_slice_ns(cpu_id);
    bool has_runnable = rq_peek_best(cpu_id) != NULL;
    bool slice_done = sched_local_slice_ns() >= target_ns;

    if (!has_runnable) {
        return rq_depth && slice_done;
    }

    return slice_done || sched_has_better(thread, cpu_id);
}

static bool tick_charges(sched_thread_t *thread) {
    if (thread == sched_local_idle()) {
        return false;
    }

    return thread_get_state(thread) == THREAD_RUNNING;
}

static void charge_thread_tick(sched_thread_t *thread, const arch_int_state_t *state) {
    if (!thread || !state) {
        return;
    }

    __atomic_fetch_add(&thread->cpu_time_ticks, 1, __ATOMIC_RELAXED);

    if (arch_signal_is_user(state)) {
        __atomic_fetch_add(&thread->user_ticks, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&thread->sys_ticks, 1, __ATOMIC_RELAXED);
    }
}

// a thread picked from the queue may still be mid-teardown on another cpu;
// switching to a half built context would jump to garbage
static bool invalid_switch_target(sched_thread_t *next, sched_thread_t *current) {
    if (!next || next == current || !thread_ctx_ok(next)) {
        return false;
    }

    return !next->context || !ctx_valid(next);
}

static bool need_irq_switch(sched_thread_t *thread, size_t cpu_id, bool force_resched, bool check_policy) {
    if (force_resched || sched_need_resched()) {
        return true;
    }

    return check_policy && should_preempt(thread, cpu_id);
}

static void keep_force(bool force_resched) {
    if (force_resched) {
        __atomic_store_n(&sched_local()->force_resched, true, __ATOMIC_RELEASE);
    }
}

static void retire_bad(sched_thread_t *thread) {
    thread_unclaim(thread);
    thread_set_state(thread, THREAD_ZOMBIE);
    thread->exit_code = -EFAULT;
    thread->exit_signal = 0;

    if (sched_state.procs.zombie_list && !thread->in_zombie_list) {
        thread->zombie_node.data = thread;
        list_append(sched_state.procs.zombie_list, &thread->zombie_node);
        thread->in_zombie_list = true;
    }

    exit_event_push(thread->pid);
}

static sched_thread_t *pick_switch_to(sched_thread_t *current, bool preempted, size_t cpu_id, unsigned long flags) {
    sched_thread_t *next = NULL;

    if (preempted) {
        next = dequeue_thread();
        if (!next) {
            thread_claim(current, cpu_id);
            sched_lock_restore(flags);
            return NULL;
        }
    } else {
        next = pick_next_thread();
    }

    while (invalid_switch_target(next, current)) {
        retire_bad(next);
        next = pick_next_thread();
    }

    if (!next || next == current) {
        thread_claim(current, cpu_id);
        sched_lock_restore(flags);
        return NULL;
    }

    return next;
}

// the outgoing thread stays owned by this cpu until its registers are saved,
// so it is published for stealing only once the switch has actually happened
static void stage_switch_away(sched_thread_t *thread, size_t cpu_id) {
    if (!thread) {
        return;
    }

    sched_thread_t *pending = __atomic_load_n(&sched_local()->handoff_ready, __ATOMIC_ACQUIRE);

    if (pending && pending != thread) {
        __atomic_store_n(&sched_local()->handoff_ready, NULL, __ATOMIC_RELEASE);
        sched_publish_handoff(pending, cpu_id);
    }

    thread_set_cpu(thread, (int)cpu_id);
    __atomic_store_n(&sched_local()->handoff_ready, thread, __ATOMIC_RELEASE);
}

static void
switch_to_thread(sched_thread_t *old, sched_thread_t *next, size_t cpu_id, unsigned long flags, bool preempted) {
    (void)preempted;
    stage_switch_away(old, cpu_id);

    sched_local_set_current(next);
    thread_claim(next, cpu_id);
    next->exec_start_ns = next->sum_exec_ns;

    if (old->fpu_initialized) {
        arch_fpu_save(old->fpu_state);
    }

    sched_lock_restore(flags);

    arch_set_kernel_stack((uintptr_t)next->stack + next->stack_size);

    if (old->vm_space != next->vm_space) {
        arch_vm_switch(next->vm_space);
    }

    if (next->fpu_initialized) {
        arch_fpu_restore(next->fpu_state);
    }

    __atomic_fetch_add(&sched_state.metrics.switch_count, 1, __ATOMIC_RELAXED);
    arch_context_switch(next->context);
}

static void irq_reschedule(arch_int_state_t *state, bool check_policy) {
    sched_thread_t *thread = sched_local_current();
    if (!state || !thread) {
        return;
    }

    sched_capture_context(state);
    __atomic_store_n(&sched_local()->resched_irq, false, __ATOMIC_RELEASE);

    bool force_resched = __atomic_exchange_n(&sched_local()->force_resched, false, __ATOMIC_ACQ_REL);

    if (sched_preempt_off()) {
        return;
    }

    if (sched_local()->sched_lock_depth > 0) {
        sched_set_resched(true);
        keep_force(force_resched);
        return;
    }

    size_t cpu_id = sched_cpu_id();
    if (!need_irq_switch(thread, cpu_id, force_resched, check_policy)) {
        return;
    }

    unsigned long flags = 0;
    if (!sched_lock_try_save(&flags)) {
        sched_set_resched(true);
        keep_force(force_resched);
        return;
    }

    sched_flush_handoff(cpu_id);
    wake_sleepers(arch_timer_ticks());

    if (!need_irq_switch(thread, cpu_id, force_resched, check_policy)) {
        sched_lock_restore(flags);
        return;
    }

    sched_set_resched(false);
    __atomic_store_n(&sched_local()->resched_irq, false, __ATOMIC_RELEASE);
    sched_set_slice_ns(0);

    if (tick_charges(thread)) {
        thread->sum_exec_ns += 1;
        thread->vruntime_ns += 1;
        thread->exec_start_ns = thread->sum_exec_ns;
    }

    bool preempted = tick_charges(thread);
    sched_thread_t *next = pick_switch_to(thread, preempted, cpu_id, flags);

    if (!next) {
        return;
    }

    switch_to_thread(thread, next, cpu_id, flags, preempted);
}

void sched_capture_context(arch_int_state_t *state) {
    if (!sched_running_get() || !state) {
        return;
    }

    sched_thread_t *current = sched_local_current();
    if (!current) {
        return;
    }

    size_t cpu_id = sched_cpu_id();
    int running_cpu = thread_cpu(current);

    if (running_cpu >= 0 && (size_t)running_cpu != cpu_id) {
        if (thread_is_owned(current)) {
            return;
        }

        thread_set_cpu(current, (int)cpu_id);
    }

    if (!ctx_state_valid(current, state)) {
        return;
    }

    current->context = (uintptr_t)state;
}

void sched_tick(arch_int_state_t *state) {
    sched_thread_t *thread = sched_local_current();

    if (!sched_running_get() || !state || !thread) {
        return;
    }

    size_t cpu_id = sched_cpu_id();

    if (__atomic_load_n(&sched_local()->handoff_ready, __ATOMIC_ACQUIRE)) {
        unsigned long flush_flags = 0;

        if (sched_lock_try_save(&flush_flags)) {
            sched_flush_handoff(cpu_id);
            sched_lock_restore(flush_flags);
        } else {
            sched_set_resched(true);
        }
    }

    __atomic_fetch_add(&sched_state.usage.total_ticks, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&sched_state.usage.core_total_ticks[cpu_id], 1, __ATOMIC_RELAXED);

    u64 now_ticks = arch_timer_ticks();
    u64 seen_wake_tick = __atomic_load_n(&sched_state.wait.sleep.wake_tick, __ATOMIC_ACQUIRE);

    if (now_ticks > seen_wake_tick) {
        unsigned long wake_flags = 0;

        if (sched_lock_try_save(&wake_flags)) {
            u64 locked_seen = __atomic_load_n(&sched_state.wait.sleep.wake_tick, __ATOMIC_ACQUIRE);

            if (now_ticks > locked_seen) {
                __atomic_store_n(&sched_state.wait.sleep.wake_tick, now_ticks, __ATOMIC_RELEASE);
                wake_sleepers(now_ticks);
            }

            sched_lock_restore(wake_flags);
        }
    }

    u64 tick_ns = sched_tick_ns();

    if (tick_charges(thread)) {
        __atomic_fetch_add(&sched_state.usage.busy_ticks, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&sched_state.usage.core_busy_ticks[cpu_id], 1, __ATOMIC_RELAXED);
        charge_thread_tick(thread, state);

        if (tick_ns) {
            thread->sum_exec_ns += tick_ns;
            thread->vruntime_ns += tick_ns;
            thread->exec_start_ns = thread->sum_exec_ns;
            sched_add_slice_ns(tick_ns);
        }
    }

    sched_capture_context(state);
    sched_signal_deliver(state);

    sched_count_local_tick();
    u64 local_ticks = sched_local_ticks();

    if (local_ticks % SCHED_REBALANCE_TICKS == (cpu_id % SCHED_REBALANCE_TICKS)) {
        unsigned long flags = 0;

        if (sched_lock_try_save(&flags)) {
            sched_rebalance_once(cpu_id);
            sched_lock_restore(flags);
        }
    }

    irq_reschedule(state, true);
}

void force_resched(void) {
    if (!sched_running_get()) {
        return;
    }

    sched_cpu_t *local = sched_local();
    __atomic_store_n(&local->force_resched, true, __ATOMIC_RELEASE);
    __atomic_store_n(&local->need_resched, true, __ATOMIC_RELEASE);

    if (sched_preempt_off() || local->sched_lock_depth || !arch_irq_enabled()) {
        return;
    }

    if (__atomic_load_n(&local->resched_irq, __ATOMIC_ACQUIRE)) {
        return;
    }

    __atomic_store_n(&local->resched_irq, true, __ATOMIC_RELEASE);
    arch_resched_self();
}

void sched_resched_local(void) {
    if (!sched_running_get()) {
        return;
    }

    sched_cpu_t *local = sched_local();
    __atomic_store_n(&local->need_resched, true, __ATOMIC_RELEASE);

    if (sched_preempt_off() || local->sched_lock_depth || !arch_irq_enabled()) {
        return;
    }

    sched_thread_t *current = sched_local_current();
    bool idle = current == sched_local_idle();
    bool running = false;
    if (current) {
        running = __atomic_load_n(&current->state, __ATOMIC_ACQUIRE) == THREAD_RUNNING;
    }

    if (running && !idle) {
        return;
    }

    if (__atomic_load_n(&local->resched_irq, __ATOMIC_ACQUIRE)) {
        return;
    }

    __atomic_store_n(&local->resched_irq, true, __ATOMIC_RELEASE);
    arch_resched_self();
}

void sched_yield(void) {
    if (!sched_running_get() || !sched_local_current()) {
        return;
    }

    if (sched_cpu_id() == 0) {
        sched_reap();
    }

    sched_set_slice_ns(sched_target_slice_ns(sched_cpu_id()));
    force_resched();
}

void sched_ipi_resched(void) {
    if (!sched_is_running()) {
        return;
    }

    sched_set_resched(true);
}

void sched_resched_softirq(arch_int_state_t *state) {
    if (!sched_is_running() || !state) {
        return;
    }

    irq_reschedule(state, true);
}

void sched_sleep(u64 ticks) {
    sched_thread_t *self = sched_local_current();
    if (!self || !ticks) {
        return;
    }

    if (!sched_running_get()) {
        u64 start = arch_timer_ticks();
        while ((arch_timer_ticks() - start) < ticks) {
            sched_spin_wait();
        }
        return;
    }

    sched_wait_deadline(arch_timer_ticks() + ticks, 0);
}

static void reparent_children(sched_thread_t *parent) {
    if (!parent || !sched_state.procs.all_list) {
        return;
    }

    sched_thread_t *reaper = NULL;

    ll_foreach(node, sched_state.procs.all_list) {
        sched_thread_t *thread = node->data;

        if (!thread || thread == parent) {
            continue;
        }

        if (thread->pid == 1) {
            reaper = thread;
            break;
        }
    }

    pid_t reaper_pid = reaper ? reaper->pid : 0;
    bool notify_reaper = false;

    ll_foreach(node, sched_state.procs.all_list) {
        sched_thread_t *thread = node->data;

        if (!thread || thread == parent) {
            continue;
        }

        if (thread->ppid != parent->pid) {
            continue;
        }

        thread->ppid = reaper_pid;

        if (reaper && thread_get_state(thread) == THREAD_ZOMBIE) {
            notify_reaper = true;
        }
    }

    if (reaper && notify_reaper) {
        sched_wake_one(&reaper->wait_queue);
        sched_signal_send(reaper, SIGCHLD);
    }
}

void sched_exit(void) {
    sched_thread_t *self = sched_local_current();
    pid_t exited_pid = 0;

    if (sched_thread_is_idle(self)) {
        panic("idle thread attempted to exit");
    }

    if (self) {
        sched_fd_close_all(self);
    }

    arch_irq_save();

    unsigned long flags = sched_lock_save();

    if (self) {
        exited_pid = self->pid;
        wq_dequeue(self);
        sleep_heap_remove(self);
        reparent_children(self);
        thread_set_state(self, THREAD_ZOMBIE);
        stage_switch_away(self, sched_cpu_id());

        if (self != sched_local_idle() && !self->in_zombie_list) {
            self->zombie_node.data = self;
            list_append(sched_state.procs.zombie_list, &self->zombie_node);
            self->in_zombie_list = true;
        }

        if (self->user_thread) {
            sched_thread_t *parent = find_thread(self->ppid);

            if (parent) {
                sched_wake_one(&parent->wait_queue);
                sched_signal_send(parent, SIGCHLD);
            }
        }
    }

    sched_thread_t *next = pick_next_thread();
    if (next) {
        sched_set_resched(false);
        __atomic_store_n(&sched_local()->force_resched, false, __ATOMIC_RELEASE);
        __atomic_store_n(&sched_local()->resched_irq, false, __ATOMIC_RELEASE);
        sched_set_slice_ns(0);
        next->exec_start_ns = next->sum_exec_ns;

        if (thread_ctx_ok(next) && (!next->context || !ctx_valid(next))) {
            sched_lock_restore(flags);
            panic("scheduler exit switched to invalid context thread");
        }

        sched_local_set_current(next);
        thread_claim(next, sched_cpu_id());
    }

    if (exited_pid > 0) {
        exit_event_push(exited_pid);
    }

    sched_lock_restore(flags);

    if (!next) {
        cpu_halt();
    }

    arch_set_kernel_stack((uintptr_t)next->stack + next->stack_size);

    if (!self || self->vm_space != next->vm_space) {
        arch_vm_switch(next->vm_space);
    }

    if (next->fpu_initialized) {
        arch_fpu_restore(next->fpu_state);
    }

    __atomic_fetch_add(&sched_state.metrics.switch_count, 1, __ATOMIC_RELAXED);
    arch_context_switch(next->context);
}

void sched_preempt_disable(void) {
    sched_preempt_inc();
}

void sched_preempt_enable(void) {
    sched_preempt_dec();
    if (!sched_preempt_off() && sched_need_resched()) {
        sched_resched_local();
    }
}

bool sched_preempt_disabled(void) {
    return sched_preempt_off();
}
