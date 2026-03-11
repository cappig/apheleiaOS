#include "scheduler_internal.h"

static void
sched_reschedule_from_interrupt(arch_int_state_t *state, bool evaluate_policy) {
    sched_thread_t *thread = sched_local_current();
    if (!state || !thread) {
        return;
    }

    sched_capture_context(state);
    __atomic_store_n(
        &sched_local()->resched_irq_pending, false, __ATOMIC_RELEASE
    );
    bool force_resched = __atomic_exchange_n(
        &sched_local()->force_resched, false, __ATOMIC_ACQ_REL
    );

    if (sched_preempt_disabled()) {
        return;
    }

    size_t cpu_id = sched_cpu_id();
    bool should_resched = force_resched || sched_local_need_resched();

    if (!should_resched && evaluate_policy) {
        size_t rq_depth = sched_rq_depth(cpu_id);
        if (
            thread == sched_local_idle() &&
            sched_thread_state_load(thread) != THREAD_RUNNING
        ) {
            sched_thread_mark_running(thread, cpu_id);
        }

        if (sched_thread_state_load(thread) != THREAD_RUNNING) {
            should_resched = true;
        } else if (thread == sched_local_idle()) {
            // If any entry exists, force dequeue path
            // so migration/recovery can run instead of idling forever.
            should_resched = rq_depth != 0;
        } else if (!sched_cpu_allowed(thread, cpu_id)) {
            should_resched = true;
        } else {
            u64 target_slice_ns = sched_target_slice_ns(cpu_id);
            bool has_runnable = rq_peek_best(cpu_id) != NULL;
            if (
                has_runnable &&
                (
                    sched_local_slice_ns() >= target_slice_ns ||
                    sched_has_better_runnable(thread, cpu_id)
                )
            ) {
                should_resched = true;
            } else if (!has_runnable && rq_depth && sched_local_slice_ns() >= target_slice_ns) {
                // Queue contains only temporarily ineligible entries; still
                // drive dequeue/steal path to keep forward progress.
                should_resched = true;
            }
        }
    }

    if (!should_resched) {
        return;
    }

    unsigned long flags = 0;
    if (!sched_lock_try_save(&flags)) {
        sched_local_set_need_resched(true);
        if (force_resched) {
            __atomic_store_n(&sched_local()->force_resched, true, __ATOMIC_RELEASE);
        }
        return;
    }

    sched_flush_handoff_ready_locked(cpu_id);
    sched_wake_sleepers_locked(arch_timer_ticks());
    should_resched = force_resched || sched_local_need_resched();

    if (!should_resched && evaluate_policy) {
        size_t rq_depth = sched_rq_depth(cpu_id);
        if (
            thread == sched_local_idle() &&
            sched_thread_state_load(thread) != THREAD_RUNNING
        ) {
            sched_thread_mark_running(thread, cpu_id);
        }

        if (sched_thread_state_load(thread) != THREAD_RUNNING) {
            should_resched = true;
        } else if (thread == sched_local_idle()) {
            should_resched = rq_depth != 0;
        } else if (!sched_cpu_allowed(thread, cpu_id)) {
            should_resched = true;
        } else {
            u64 target_slice_ns = sched_target_slice_ns(cpu_id);
            bool has_runnable = rq_peek_best(cpu_id) != NULL;
            if (
                has_runnable &&
                (
                    sched_local_slice_ns() >= target_slice_ns ||
                    sched_has_better_runnable(thread, cpu_id)
                )
            ) {
                should_resched = true;
            } else if (!has_runnable && rq_depth && sched_local_slice_ns() >= target_slice_ns) {
                should_resched = true;
            }
        }
    }

    if (!should_resched) {
        sched_lock_restore(flags);
        return;
    }

    sched_local_set_need_resched(false);
    __atomic_store_n(
        &sched_local()->resched_irq_pending, false, __ATOMIC_RELEASE
    );
    sched_local_set_slice_ns(0);

    sched_thread_t *next = NULL;
    bool preempted_running = (
        sched_thread_state_load(thread) == THREAD_RUNNING &&
        thread != sched_local_idle()
    );

    if (preempted_running) {
        next = dequeue_thread();
        if (!next) {
            sched_thread_mark_running(thread, cpu_id);
            sched_lock_restore(flags);
            return;
        }
    } else {
        next = pick_next_thread();
    }

    while (
        SCHED_STRICT_CONTEXT_CHECK && next && next != thread &&
        sched_context_checked_thread(next) &&
        (!next->context || !sched_context_valid(next))
    ) {
        const char *reason = NULL;
        (void)sched_context_valid_ex(next, &reason);
        sched_log_invalid_context_detail(next, reason);
        log_warn(
            "scheduler dropping invalid next pid=%ld (%s) state=%d reason=%s",
            (long)next->pid,
            next->name,
            sched_thread_state_load(next),
            reason ? reason : "unknown"
        );

        sched_thread_mark_not_running(next);
        sched_thread_state_store(next, THREAD_ZOMBIE);
        next->exit_code = -EFAULT;

        if (sched_state.zombie_list && !next->in_zombie_list) {
            next->zombie_node.data = next;
            list_append(sched_state.zombie_list, &next->zombie_node);
            next->in_zombie_list = true;
        }
        sched_exit_event_push(next->pid);

        next = pick_next_thread();
    }

    if (!next || next == thread) {
        sched_thread_mark_running(thread, cpu_id);
        sched_lock_restore(flags);
        return;
    }

    if (preempted_running) {
        // do not enqueue the currently running thread before we actually
        // switch away; otherwise another core can run it concurrently
        sched_thread_t *pending = sched_local()->handoff_ready;

        if (pending && pending != thread) {
            sched_local()->handoff_ready = NULL;
            sched_publish_handoff_thread_locked(pending, cpu_id);
        }

        sched_local()->handoff_ready = thread;
    } else {
        sched_local()->handoff_ready = thread;
    }

    sched_local_set_current(next);
    sched_thread_mark_running(next, cpu_id);
    next->exec_start_ns = next->sum_exec_ns;
    sched_lock_restore(flags);

    if (thread->fpu_initialized) {
        arch_fpu_save(thread->fpu_state);
    }

    arch_set_kernel_stack((uintptr_t)next->stack + next->stack_size);
    arch_vm_switch(next->vm_space);

    if (next->fpu_initialized) {
        arch_fpu_restore(next->fpu_state);
    }

    __atomic_fetch_add(&sched_state.metrics.switch_count, 1, __ATOMIC_RELAXED);
    arch_context_switch(next->context);
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
    int running_cpu = sched_thread_running_cpu_load(current);
    if (running_cpu >= 0 && (size_t)running_cpu != cpu_id) {
        sched_note_ownership_conflict("sched_capture_context", current);
        sched_thread_running_cpu_store(current, (int)cpu_id);
    }

    if (!sched_context_candidate_valid(current, state)) {
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
        // Preempted RUNNING tasks are deferred until after context switch
        unsigned long flush_flags = 0;
        if (sched_lock_try_save(&flush_flags)) {
            sched_flush_handoff_ready_locked(cpu_id);
            sched_lock_restore(flush_flags);
        }
    }

    __atomic_fetch_add(&sched_state.usage.total_ticks, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&sched_state.usage.core_total_ticks[cpu_id], 1, __ATOMIC_RELAXED);

    u64 now_ticks = arch_timer_ticks();
    u64 seen_wake_tick =
        __atomic_load_n(&sched_state.sleep.wake_tick, __ATOMIC_ACQUIRE);
    while (now_ticks > seen_wake_tick) {
        u64 expected = seen_wake_tick;
        if (__atomic_compare_exchange_n(
                &sched_state.sleep.wake_tick,
                &expected,
                now_ticks,
                false,
                __ATOMIC_ACQ_REL,
                __ATOMIC_ACQUIRE
            )) {
            sched_wake_sleepers(now_ticks);
            break;
        }
        seen_wake_tick = expected;
    }

    u64 tick_ns = sched_tick_ns();

    if (
        thread != sched_local_idle() &&
        sched_thread_state_load(thread) == THREAD_RUNNING
    ) {
        __atomic_fetch_add(&sched_state.usage.busy_ticks, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&sched_state.usage.core_busy_ticks[cpu_id], 1, __ATOMIC_RELAXED);
        __sync_fetch_and_add(&thread->cpu_time_ticks, 1);

        if (tick_ns) {
            thread->sum_exec_ns += tick_ns;
            thread->vruntime_ns += tick_ns;
            thread->exec_start_ns = thread->sum_exec_ns;
            sched_local_add_slice_ns(tick_ns);
        }
    }

    sched_capture_context(state);
    sched_signal_deliver_current(state);

    sched_local_inc_local_ticks();
    u64 local_ticks = sched_local_ticks();
    if (local_ticks % SCHED_REBALANCE_TICKS == (cpu_id % SCHED_REBALANCE_TICKS)) {
        unsigned long flags = 0;
        if (sched_lock_try_save(&flags)) {
            sched_rebalance_once(cpu_id);
            sched_lock_restore(flags);
        }
    }

    sched_reschedule_from_interrupt(state, true);
}

void sched_request_resched_local_force(void) {
    if (!sched_running_get()) {
        return;
    }

    sched_cpu_state_t *local = sched_local();
    __atomic_store_n(&local->force_resched, true, __ATOMIC_RELEASE);
    __atomic_store_n(&local->need_resched, true, __ATOMIC_RELEASE);

    if (sched_preempt_disabled()) {
        return;
    }

    if (local->sched_lock_depth) {
        return;
    }

    if (!arch_irq_enabled()) {
        return;
    }

    if (__atomic_load_n(&local->resched_irq_pending, __ATOMIC_ACQUIRE)) {
        return;
    }

    __atomic_store_n(&local->resched_irq_pending, true, __ATOMIC_RELEASE);
    arch_sched_request_resched();
}

void sched_request_resched_local(void) {
    if (!sched_running_get()) {
        return;
    }

    sched_cpu_state_t *local = sched_local();
    __atomic_store_n(&local->need_resched, true, __ATOMIC_RELEASE);

    if (sched_preempt_disabled()) {
        return;
    }

    if (local->sched_lock_depth) {
        return;
    }

    if (!arch_irq_enabled()) {
        return;
    }

    sched_thread_t *current = sched_local_current();
    if (
        current &&
        current != sched_local_idle() &&
        __atomic_load_n(&current->state, __ATOMIC_ACQUIRE) == THREAD_RUNNING
    ) {
        return;
    }

    if (__atomic_load_n(&local->resched_irq_pending, __ATOMIC_ACQUIRE)) {
        return;
    }

    __atomic_store_n(&local->resched_irq_pending, true, __ATOMIC_RELEASE);
    arch_sched_request_resched();
}

void sched_lockdep_note_block_under_spin(void) {
    __atomic_fetch_add(&sched_state.metrics.lockdep_block_under_spin_count, 1, __ATOMIC_RELAXED);
}

void sched_yield(void) {
    if (!sched_running_get() || !sched_local_current()) {
        return;
    }

    if (sched_cpu_id() == 0) {
        sched_reap();
    }

    sched_local_set_slice_ns(sched_target_slice_ns(sched_cpu_id()));
    sched_request_resched_local_force();
}

void sched_ipi_resched(void) {
    if (!sched_is_running()) {
        return;
    }

    sched_local_set_need_resched(true);
}

void sched_resched_softirq(arch_int_state_t *state) {
    if (!sched_is_running() || !state) {
        return;
    }

    sched_reschedule_from_interrupt(state, true);
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

    u64 deadline = arch_timer_ticks() + ticks;
    (void)sched_wait_deadline(deadline, 0);
}

static void sched_reparent_children(sched_thread_t *parent) {
    if (!parent || !sched_state.all_list) {
        return;
    }

    sched_thread_t *reaper = NULL;

    ll_foreach(node, sched_state.all_list) {
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

    ll_foreach(node, sched_state.all_list) {
        sched_thread_t *thread = node->data;

        if (!thread || thread == parent) {
            continue;
        }

        if (thread->ppid != parent->pid) {
            continue;
        }

        thread->ppid = reaper_pid;

        if (reaper && sched_thread_state_load(thread) == THREAD_ZOMBIE) {
            notify_reaper = true;
        }
    }

    if (reaper && notify_reaper) {
        sched_wake_one(&reaper->wait_queue);
        sched_signal_send_thread(reaper, SIGCHLD);
    }
}

void sched_exit(void) {
    arch_irq_disable();
    sched_thread_t *self = sched_local_current();
    pid_t exited_pid = 0;

    if (sched_thread_is_idle(self)) {
        panic("idle thread attempted to exit");
    }

    unsigned long flags = sched_lock_save();

    if (self) {
        exited_pid = self->pid;
        wait_queue_remove_locked(self);
        sleep_heap_remove(self);
        sched_reparent_children(self);
        sched_thread_mark_not_running(self);
        sched_thread_state_store(self, THREAD_ZOMBIE);

        if (self != sched_local_idle() && !self->in_zombie_list) {
            self->zombie_node.data = self;
            list_append(sched_state.zombie_list, &self->zombie_node);
            self->in_zombie_list = true;
        }

        if (self->user_thread) {
            sched_thread_t *parent = find_thread_by_pid_locked(self->ppid);

            if (parent) {
                sched_wake_one(&parent->wait_queue);
                (void)sched_signal_send_thread(parent, SIGCHLD);
            }
        }
    }

    sched_thread_t *next = pick_next_thread();
    if (next) {
        sched_local_set_need_resched(false);
        __atomic_store_n(&sched_local()->force_resched, false, __ATOMIC_RELEASE);
        __atomic_store_n(
            &sched_local()->resched_irq_pending, false, __ATOMIC_RELEASE
        );
        sched_local_set_slice_ns(0);
        sched_local_set_current(next);
        sched_thread_mark_running(next, sched_cpu_id());
        next->exec_start_ns = next->sum_exec_ns;
    }

    sched_lock_restore(flags);

    if (exited_pid > 0) {
        sched_exit_event_push(exited_pid);
    }

    if (!next) {
        cpu_halt();
    }

    if (
        SCHED_STRICT_CONTEXT_CHECK && sched_context_checked_thread(next) &&
        (!next->context || !sched_context_valid(next))
    ) {
        panic("scheduler exit switched to invalid context thread");
    }

    arch_set_kernel_stack((uintptr_t)next->stack + next->stack_size);
    arch_vm_switch(next->vm_space);

    if (next->fpu_initialized) {
        arch_fpu_restore(next->fpu_state);
    }

    __atomic_fetch_add(&sched_state.metrics.switch_count, 1, __ATOMIC_RELAXED);
    arch_context_switch(next->context);
}

void sched_preempt_disable(void) {
    sched_local_inc_preempt_depth();
}

void sched_preempt_enable(void) {
    sched_local_dec_preempt_depth();

    if (!sched_local_preempt_disabled() && sched_local_need_resched()) {
        sched_request_resched_local();
    }
}

bool sched_preempt_disabled(void) {
    return sched_local_preempt_disabled();
}
