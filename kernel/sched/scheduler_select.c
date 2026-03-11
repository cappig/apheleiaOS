#include "scheduler_internal.h"

size_t sched_cpu_load(size_t cpu_id) {
    if (cpu_id >= MAX_CORES) {
        return 0;
    }

    sched_rq_t *rq = &sched_state.runqueues[cpu_id];
    size_t load = __atomic_load_n(&rq->nr_running, __ATOMIC_RELAXED);
    sched_thread_t *current = __atomic_load_n(
        &sched_state.cpu[cpu_id].current, __ATOMIC_ACQUIRE
    );
    sched_thread_t *idle = __atomic_load_n(
        &sched_state.cpu[cpu_id].idle_thread, __ATOMIC_ACQUIRE
    );

    if (current && current != idle) {
        load++;
    }

    return load;
}

size_t sched_rq_depth(size_t cpu_id) {
    if (cpu_id >= MAX_CORES) {
        return 0;
    }

    return __atomic_load_n(&sched_state.runqueues[cpu_id].nr_running, __ATOMIC_RELAXED);
}

bool sched_cpu_needs_wake_ipi(size_t cpu_id) {
    if (cpu_id >= MAX_CORES) {
        return false;
    }

    sched_thread_t *current = __atomic_load_n(
        &sched_state.cpu[cpu_id].current, __ATOMIC_ACQUIRE
    );
    sched_thread_t *idle = __atomic_load_n(
        &sched_state.cpu[cpu_id].idle_thread, __ATOMIC_ACQUIRE
    );

    return !current || current == idle;
}

size_t
sched_cpu_distance(size_t from_cpu, size_t to_cpu, size_t cpu_count) {
    if (!cpu_count || from_cpu >= cpu_count || to_cpu >= cpu_count) {
        return (size_t)-1;
    }

    size_t direct =
        from_cpu > to_cpu ? (from_cpu - to_cpu) : (to_cpu - from_cpu);
    size_t wrap = cpu_count - direct;
    return direct < wrap ? direct : wrap;
}

size_t sched_pick_allowed_cpu_minload(
    const sched_thread_t *thread,
    size_t fallback_cpu
) {
    size_t ncpu = core_count;
    if (ncpu > MAX_CORES) {
        ncpu = MAX_CORES;
    }

    // FIXME: this should not be a hard cap
    if (ncpu > 64) {
        ncpu = 64;
    }

    if (!ncpu) {
        ncpu = 1;
    }

    u64 online = sched_online_cpu_mask();
    u64 allowed = thread ? thread->allowed_cpu_mask : 0;
    if (!allowed) {
        allowed = online;
    }

    allowed &= online;
    if (!allowed) {
        if (fallback_cpu < ncpu) {
            return fallback_cpu;
        }
        return 0;
    }

    size_t best_cpu = MAX_CORES;
    size_t best_load = (size_t)-1;

    for (size_t cpu = 0; cpu < ncpu; cpu++) {
        if (!(allowed & (1ULL << cpu))) {
            continue;
        }

        if (!cores_local[cpu].valid || !cores_local[cpu].online) {
            continue;
        }

        size_t load = sched_cpu_load(cpu);
        if (
            best_cpu >= MAX_CORES || load < best_load ||
            (load == best_load && cpu == fallback_cpu)
        ) {
            best_cpu = cpu;
            best_load = load;
        }
    }

    if (best_cpu < MAX_CORES) {
        return best_cpu;
    }

    if (fallback_cpu < ncpu && (allowed & (1ULL << fallback_cpu))) {
        return fallback_cpu;
    }

    for (size_t cpu = 0; cpu < ncpu; cpu++) {
        if (allowed & (1ULL << cpu)) {
            return cpu;
        }
    }

    return 0;
}

void sched_nudge_owned_thread_locked(sched_thread_t *thread) {
    int running_cpu = sched_thread_running_cpu_load(thread);
    if (!thread || running_cpu < 0 || (size_t)running_cpu >= MAX_CORES) {
        return;
    }

    bool current_owned = sched_thread_owned_by_current_cpu(thread);
    bool handoff_owned = sched_thread_owned_in_handoff(thread);
    if (!current_owned && !handoff_owned) {
        return;
    }

    if (current_owned && sched_thread_state_load(thread) == THREAD_SLEEPING) {
        sched_thread_state_store(thread, THREAD_RUNNING);
    }

    size_t owner_cpu = (size_t)running_cpu;
    if (owner_cpu == sched_cpu_id()) {
        sched_request_resched_local();
    } else if (sched_send_wake_ipi(owner_cpu)) {
        __atomic_fetch_add(&sched_state.metrics.wake_ipi, 1, __ATOMIC_RELAXED);
    }
}

void sched_publish_handoff_thread_locked(
    sched_thread_t *thread,
    size_t owner_cpu
) {
    if (!thread || owner_cpu >= MAX_CORES) {
        return;
    }

    sched_thread_mark_not_running(thread);

    if (sched_thread_state_load(thread) == THREAD_RUNNING) {
        sched_thread_state_store(thread, THREAD_READY);
    } else if (
        sched_thread_state_load(thread) == THREAD_SLEEPING &&
        thread->wait_result != (u8)SCHED_WAIT_ABORTED &&
        !thread->in_wait_queue &&
        !thread->sleep_queued
    ) {
        sched_thread_state_store(thread, THREAD_READY);
    }

    if (
        sched_thread_state_load(thread) != THREAD_READY ||
        thread->on_rq ||
        !thread->context
    ) {
        return;
    }

    size_t target_cpu = owner_cpu;
    if (!sched_cpu_allowed(thread, owner_cpu)) {
        target_cpu = sched_pick_allowed_cpu_minload(thread, owner_cpu);
    }

    rq_enqueue_cpu(thread, target_cpu);

    if (target_cpu != owner_cpu) {
        if (sched_send_wake_ipi(target_cpu)) {
            __atomic_fetch_add(&sched_state.metrics.wake_ipi, 1, __ATOMIC_RELAXED);
        }
    }
}

void sched_flush_handoff_ready_locked(size_t cpu_id) {
    if (cpu_id >= MAX_CORES) {
        return;
    }

    sched_cpu_state_t *local = &sched_state.cpu[cpu_id];
    sched_thread_t *pending = local->handoff_ready;
    if (!pending) {
        return;
    }

    local->handoff_ready = NULL;
    sched_publish_handoff_thread_locked(pending, cpu_id);
}

void sched_runqueue_record_depth(size_t depth) {
    u64 prior = __sync_fetch_and_add(&sched_state.metrics.runqueue_max, 0);
    while ((u64)depth > prior) {
        u64 observed =
            __sync_val_compare_and_swap(&sched_state.metrics.runqueue_max, prior, (u64)depth);

        if (observed == prior) {
            return;
        }

        prior = observed;
    }
}
