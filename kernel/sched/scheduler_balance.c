#include "scheduler_internal.h"

u64 sched_target_slice_ns(size_t cpu_id) {
    size_t runnable = sched_cpu_load(cpu_id);
    if (!runnable) {
        runnable = 1;
    }

    u64 slice = SCHED_LATENCY_NS / (u64)runnable;
    if (slice < SCHED_MIN_GRANULARITY_NS) {
        slice = SCHED_MIN_GRANULARITY_NS;
    }

    return slice;
}

bool sched_has_better_runnable(sched_thread_t *current, size_t cpu_id) {
    if (!current || cpu_id >= MAX_CORES) {
        return false;
    }

    sched_thread_t *best = rq_peek_best(cpu_id);
    if (!best) {
        return false;
    }

    if (best->vruntime_ns < current->vruntime_ns) {
        return true;
    }

    if (best->vruntime_ns == current->vruntime_ns && best->pid < current->pid) {
        return true;
    }

    return false;
}

static size_t sched_push_load(size_t source_cpu, size_t target_cpu, size_t max_moves) {
    if (
        source_cpu >= MAX_CORES || target_cpu >= MAX_CORES || source_cpu == target_cpu ||
        !max_moves
    ) {
        return 0;
    }

    size_t moved = 0;
    for (size_t i = 0; i < max_moves; i++) {
        sched_thread_t *candidate =
            rq_pop_worst_allowed_from_cpu(source_cpu, target_cpu);
        if (!candidate) {
            break;
        }

        candidate->last_cpu = target_cpu;
        candidate->affinity_core = target_cpu;
        rq_enqueue_cpu(candidate, target_cpu);
        moved++;
        __atomic_fetch_add(&sched_state.metrics.migrations, 1, __ATOMIC_RELAXED);

        if (sched_send_wake_ipi(target_cpu)) {
            __atomic_fetch_add(&sched_state.metrics.wake_ipi, 1, __ATOMIC_RELAXED);
        }
    }

    return moved;
}

void sched_rebalance_once(size_t cpu_id) {
    size_t ncpu = core_count;
    if (ncpu > MAX_CORES) {
        ncpu = MAX_CORES;
    }
    if (ncpu > 64) {
        ncpu = 64;
    }

    if (ncpu <= 1) {
        return;
    }

    __atomic_fetch_add(&sched_state.metrics.balance_runs, 1, __ATOMIC_RELAXED);

    for (size_t moved = 0; moved < SCHED_PUSH_BATCH; moved++) {
        sched_thread_t *stranded = rq_pop_disallowed_from_cpu(cpu_id, cpu_id);
        if (!stranded) {
            break;
        }

        size_t target_cpu = sched_pick_allowed_cpu_minload(stranded, cpu_id);
        if (
            target_cpu >= MAX_CORES || target_cpu == cpu_id ||
            !sched_cpu_allowed(stranded, target_cpu)
        ) {
            rq_enqueue_cpu(stranded, cpu_id);
            break;
        }

        stranded->last_cpu = target_cpu;
        stranded->affinity_core = target_cpu;
        rq_enqueue_cpu(stranded, target_cpu);
        __atomic_fetch_add(&sched_state.metrics.migrations, 1, __ATOMIC_RELAXED);
        if (sched_send_wake_ipi(target_cpu)) {
            __atomic_fetch_add(&sched_state.metrics.wake_ipi, 1, __ATOMIC_RELAXED);
        }
    }

    size_t idlest = MAX_CORES;
    size_t idlest_load = (size_t)-1;
    size_t local_load = sched_cpu_load(cpu_id);

    for (size_t cpu = 0; cpu < ncpu; cpu++) {
        if (!cores_local[cpu].valid || !cores_local[cpu].online) {
            continue;
        }

        size_t load = sched_cpu_load(cpu);
        if (load < idlest_load) {
            idlest = cpu;
            idlest_load = load;
        }
    }

    if (idlest >= MAX_CORES || cpu_id >= MAX_CORES || cpu_id == idlest) {
        return;
    }

    if (local_load <= (idlest_load + 1)) {
        return;
    }

    size_t overload = local_load - idlest_load;
    size_t max_moves = overload / 2;
    if (!max_moves) {
        max_moves = 1;
    }
    if (max_moves > SCHED_PUSH_BATCH) {
        max_moves = SCHED_PUSH_BATCH;
    }

    (void)sched_push_load(cpu_id, idlest, max_moves);
}
