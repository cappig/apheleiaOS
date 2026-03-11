#include "scheduler_internal.h"

sched_thread_t *dequeue_thread(void) {
    size_t cpu_id = sched_cpu_id();
    sched_flush_handoff_ready_locked(cpu_id);

    for (;;) {
        sched_thread_t *thread = rq_pop_best_allowed(cpu_id);
        if (!thread) {
            break;
        }

        if (!thread->context) {
            if (sched_thread_state_load(thread) == THREAD_READY && !thread->on_rq) {
                enqueue_thread(thread);
            }
            continue;
        }

        if (
            SCHED_STRICT_CONTEXT_CHECK && sched_context_checked_thread(thread) &&
            !sched_context_valid(thread)
        ) {
            const char *reason = NULL;
            (void)sched_context_valid_ex(thread, &reason);
            sched_log_invalid_context_detail(thread, reason);
            log_warn(
                "scheduler dropped invalid context runnable pid=%ld (%s) reason=%s",
                (long)thread->pid,
                thread->name,
                reason ? reason : "unknown"
            );

            sched_thread_mark_not_running(thread);
            sched_thread_state_store(thread, THREAD_ZOMBIE);
            thread->exit_code = -EFAULT;

            if (sched_state.zombie_list && !thread->in_zombie_list) {
                thread->zombie_node.data = thread;
                list_append(sched_state.zombie_list, &thread->zombie_node);
                thread->in_zombie_list = true;
            }
            sched_exit_event_push(thread->pid);
            continue;
        }

        if (sched_thread_state_load(thread) != THREAD_READY) {
            if (
                sched_thread_state_load(thread) == THREAD_RUNNING &&
                !sched_thread_owned_by_running_cpu(thread)
            ) {
                sched_note_ownership_conflict("dequeue_thread", thread);
            }
            continue;
        }

        if (sched_thread_running_cpu_load(thread) >= 0) {
            if (sched_thread_owned_by_running_cpu(thread)) {
                sched_note_ownership_conflict("dequeue_thread_ready_owned", thread);
                continue;
            }

            sched_thread_running_cpu_store(thread, -1);
        }

        return thread;
    }

    sched_thread_t *stranded = rq_pop_disallowed_from_cpu(cpu_id, cpu_id);
    if (stranded) {
        size_t target_cpu = sched_pick_allowed_cpu_minload(stranded, cpu_id);
        if (
            target_cpu < MAX_CORES &&
            target_cpu != cpu_id &&
            sched_cpu_allowed(stranded, target_cpu) &&
            cores_local[target_cpu].online
        ) {
            stranded->last_cpu = target_cpu;
            stranded->affinity_core = target_cpu;
            rq_enqueue_cpu(stranded, target_cpu);
            __atomic_fetch_add(&sched_state.metrics.migrations, 1, __ATOMIC_RELAXED);
            if (sched_send_wake_ipi(target_cpu)) {
                __atomic_fetch_add(&sched_state.metrics.wake_ipi, 1, __ATOMIC_RELAXED);
            }
        } else {
            rq_enqueue_cpu(stranded, cpu_id);
        }

        sched_thread_t *rescued = rq_pop_best_allowed(cpu_id);
        if (rescued) {
            return rescued;
        }
    }

    size_t busiest_cpu = MAX_CORES;
    size_t busiest_load = 0;

    for (size_t i = 0; i < core_count && i < MAX_CORES; i++) {
        if (i == cpu_id || !cores_local[i].valid || !cores_local[i].online) {
            continue;
        }

        size_t load = sched_cpu_load(i);
        if (load > busiest_load) {
            busiest_load = load;
            busiest_cpu = i;
        }
    }

    if (busiest_cpu >= MAX_CORES || busiest_load < 2) {
        return NULL;
    }

    sched_thread_t *first = NULL;
    for (size_t i = 0; i < SCHED_IDLE_STEAL_BATCH; i++) {
        sched_thread_t *victim =
            rq_pop_worst_allowed_from_cpu(busiest_cpu, cpu_id);
        if (!victim) {
            break;
        }

        victim->last_cpu = cpu_id;
        victim->affinity_core = cpu_id;
        __atomic_fetch_add(&sched_state.metrics.steals, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&sched_state.metrics.migrations, 1, __ATOMIC_RELAXED);

        if (!first) {
            first = victim;
            continue;
        }

        rq_enqueue_cpu(victim, cpu_id);
    }

    return first;
}

sched_thread_t *pick_next_thread(void) {
    sched_thread_t *next = dequeue_thread();
    if (next) {
        return next;
    }

    if (sched_local_idle()) {
        return sched_local_idle();
    }

    return sched_local_current();
}

sched_thread_t *pick_bootstrap_init_thread(void) {
    size_t cpu_id = sched_cpu_id();

    for (size_t i = 0; i < core_count && i < MAX_CORES; i++) {
        sched_rq_t *rq = &sched_state.runqueues[i];
        unsigned long flags = spin_lock_irqsave(&rq->lock);

        for (u32 j = 0; (size_t)j < rq->nr_running; j++) {
            sched_thread_t *thread = rq->heap[j];

            if (
                !thread || thread->pid != 1 ||
                sched_thread_state_load(thread) != THREAD_READY || !thread->context
            ) {
                continue;
            }

            if (SCHED_STRICT_CONTEXT_CHECK && !sched_context_valid(thread)) {
                continue;
            }

            (void)rq_remove_index_locked(rq, j);
            spin_unlock_irqrestore(&rq->lock, flags);

            thread->last_cpu = cpu_id;
            thread->affinity_core = cpu_id;
            return thread;
        }

        spin_unlock_irqrestore(&rq->lock, flags);
    }

    return NULL;
}
