#include "internal.h"

static void make_zombie(sched_thread_t *thread, int code) {
    thread_unclaim(thread);
    thread_set_state(thread, THREAD_ZOMBIE);
    thread->exit_code = code;
    thread->exit_signal = 0;

    if (sched_state.procs.zombie_list && !thread->in_zombie_list) {
        thread->zombie_node.data = thread;
        list_append(sched_state.procs.zombie_list, &thread->zombie_node);
        thread->in_zombie_list = true;
    }

    exit_event_push(thread->pid);
}

static bool ready_to_run(sched_thread_t *thread) {
    if (!thread->context) {
        if (thread_get_state(thread) == THREAD_READY && !thread->on_rq) {
            enqueue_thread(thread);
        }

        return false;
    }

    bool bad_context = thread_ctx_ok(thread) && !ctx_valid(thread);
    if (bad_context) {
        make_zombie(thread, -EFAULT);
        return false;
    }

    if (thread_get_state(thread) != THREAD_READY) {
        bool lost_running_thread = thread_get_state(thread) == THREAD_RUNNING && !thread_is_owned(thread);

        if (lost_running_thread) {
            sched_repair_thread(thread, true);
        }

        return false;
    }

    if (thread_cpu(thread) < 0) {
        return true;
    }

    if (thread_is_owned(thread)) {
        return false;
    }

    thread_set_cpu(thread, -1);
    return true;
}

static sched_thread_t *pick_local(size_t cpu_id) {
    sched_thread_t *thread = NULL;

    while ((thread = rq_pop_best_allowed(cpu_id))) {
        if (ready_to_run(thread)) {
            return thread;
        }
    }

    return NULL;
}

static sched_thread_t *rescue_stranded(size_t cpu_id) {
    sched_thread_t *stranded = rq_pop_disallowed_from_cpu(cpu_id, cpu_id);
    if (!stranded) {
        return NULL;
    }

    size_t target_cpu = pick_cpu(stranded, cpu_id);
    bool can_move = target_cpu < MAX_CORES && target_cpu != cpu_id && sched_cpu_allowed(stranded, target_cpu) &&
                    cores_local[target_cpu].online;

    if (!can_move) {
        rq_enqueue_cpu(stranded, cpu_id);
        return rq_pop_best_allowed(cpu_id);
    }

    stranded->last_cpu = target_cpu;
    stranded->affinity_core = target_cpu;
    rq_enqueue_cpu(stranded, target_cpu);
    __atomic_fetch_add(&sched_state.metrics.migrations, 1, __ATOMIC_RELAXED);

    if (wake_cpu(target_cpu)) {
        __atomic_fetch_add(&sched_state.metrics.wake_ipi, 1, __ATOMIC_RELAXED);
    }

    return rq_pop_best_allowed(cpu_id);
}

static size_t busiest_peer(size_t cpu_id, size_t *load_out) {
    size_t cpu = MAX_CORES;
    size_t best_load = 0;

    for (size_t i = 0; i < core_count && i < MAX_CORES; i++) {
        if (i == cpu_id || !cores_local[i].valid || !cores_local[i].online) {
            continue;
        }

        size_t load = sched_cpu_load(i);
        if (load > best_load) {
            best_load = load;
            cpu = i;
        }
    }

    *load_out = best_load;
    return cpu;
}

static sched_thread_t *steal_idle_work(size_t cpu_id) {
    size_t load = 0;
    size_t cpu = busiest_peer(cpu_id, &load);

    if (cpu >= MAX_CORES || load < 2) {
        return NULL;
    }

    sched_thread_t *first = NULL;

    for (size_t i = 0; i < SCHED_IDLE_STEAL_BATCH; i++) {
        sched_thread_t *victim = rq_pop_worst_allowed_from_cpu(cpu, cpu_id);

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

sched_thread_t *dequeue_thread(void) {
    size_t cpu_id = sched_cpu_id();
    sched_flush_handoff(cpu_id);

    sched_thread_t *thread = pick_local(cpu_id);
    if (thread) {
        return thread;
    }

    thread = rescue_stranded(cpu_id);
    if (thread) {
        return thread;
    }

    return steal_idle_work(cpu_id);
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

sched_thread_t *pick_init_thread(void) {
    size_t cpu_id = sched_cpu_id();

    for (size_t i = 0; i < core_count && i < MAX_CORES; i++) {
        sched_rq_t *rq = &sched_state.cpus.runqueues[i];
        unsigned long flags = spin_lock_irqsave(&rq->lock);

        for (u32 j = 0; (size_t)j < rq->nr_running; j++) {
            sched_thread_t *thread = rq->heap[j];
            bool not_init =
                (!thread || thread->pid != 1 || thread_get_state(thread) != THREAD_READY || !thread->context);

            if (not_init) {
                continue;
            }

            if (!ctx_valid(thread)) {
                continue;
            }

            rq_remove_index(rq, j);
            spin_unlock_irqrestore(&rq->lock, flags);

            thread->last_cpu = cpu_id;
            thread->affinity_core = cpu_id;

            return thread;
        }

        spin_unlock_irqrestore(&rq->lock, flags);
    }

    return NULL;
}
