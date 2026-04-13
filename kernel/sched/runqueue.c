#include "internal.h"

static inline bool rq_less(const sched_thread_t *a, const sched_thread_t *b) {
    if (!a) {
        return false;
    }

    if (!b) {
        return true;
    }

    if (a->vruntime_ns != b->vruntime_ns) {
        return a->vruntime_ns < b->vruntime_ns;
    }

    if (a->sum_exec_ns != b->sum_exec_ns) {
        return a->sum_exec_ns < b->sum_exec_ns;
    }

    return a->pid < b->pid;
}

static inline void rq_swap(sched_rq_t *rq, u32 left, u32 right) {
    sched_thread_t *tmp = rq->heap[left];
    rq->heap[left] = rq->heap[right];
    rq->heap[right] = tmp;

    if (rq->heap[left]) {
        rq->heap[left]->rq_index = left;
    }

    if (rq->heap[right]) {
        rq->heap[right]->rq_index = right;
    }
}

static void rq_sift_up(sched_rq_t *rq, u32 index) {
    while (index > 0) {
        u32 parent = (index - 1U) / 2U;

        if (!rq_less(rq->heap[index], rq->heap[parent])) {
            break;
        }

        rq_swap(rq, index, parent);
        index = parent;
    }
}

static void rq_sift_down(sched_rq_t *rq, u32 index) {
    for (;;) {
        u32 left = index * 2U + 1U;
        u32 right = left + 1U;
        u32 best = index;

        if ((size_t)left < rq->nr_running && rq_less(rq->heap[left], rq->heap[best])) {
            best = left;
        }

        if ((size_t)right < rq->nr_running && rq_less(rq->heap[right], rq->heap[best])) {
            best = right;
        }

        if (best == index) {
            return;
        }

        rq_swap(rq, index, best);
        index = best;
    }
}

static bool rq_insert(sched_rq_t *rq, sched_thread_t *thread) {
    if (!rq || !thread || !rq->heap || rq->nr_running >= rq->capacity) {
        return false;
    }

    u32 index = (u32)rq->nr_running;

    rq->heap[index] = thread;
    thread->rq_index = index;
    rq->nr_running++;

    rq_sift_up(rq, index);

    return true;
}

static int rq_find_index(
    const sched_rq_t *rq,
    const sched_thread_t *thread
) {
    if (!rq || !thread || !rq->heap) {
        return -1;
    }

    for (size_t i = 0; i < rq->nr_running; i++) {
        if (rq->heap[i] == thread) {
            return (int)i;
        }
    }

    return -1;
}

bool rq_remove_index(sched_rq_t *rq, u32 index) {
    if (!rq || !rq->heap || (size_t)index >= rq->nr_running) {
        return false;
    }

    size_t last = rq->nr_running - 1U;

    sched_thread_t *removed = rq->heap[index];
    if (!removed) {
        return false;
    }

    if ((size_t)index != last) {
        rq->heap[index] = rq->heap[last];

        if (rq->heap[index]) {
            rq->heap[index]->rq_index = index;
        }
    }

    rq->heap[last] = NULL;
    rq->nr_running = last;

    removed->rq_index = UINT32_MAX;
    removed->on_rq = false;
    removed->in_run_queue = false;

    if ((size_t)index < rq->nr_running) {
        if (index > 0U) {
            u32 parent = (index - 1U) / 2U;

            if (rq_less(rq->heap[index], rq->heap[parent])) {
                rq_sift_up(rq, index);

                if (rq->nr_running > 0 && rq->heap[0]) {
                    rq->min_vruntime = rq->heap[0]->vruntime_ns;
                }

                return true;
            }
        }

        rq_sift_down(rq, index);
    }

    if (rq->nr_running > 0 && rq->heap[0]) {
        rq->min_vruntime = rq->heap[0]->vruntime_ns;
    }

    return true;
}

void rq_enqueue_cpu(sched_thread_t *thread, size_t cpu_id) {
    if (!thread || cpu_id >= MAX_CORES) {
        return;
    }

    if (!sched_cpu_allowed(thread, cpu_id) || !cores_local[cpu_id].online) {
        size_t allowed_cpu = pick_cpu(thread, cpu_id);

        if (
            allowed_cpu >= MAX_CORES ||
            !cores_local[allowed_cpu].online ||
            !sched_cpu_allowed(thread, allowed_cpu)
        ) {
            return;
        }

        cpu_id = allowed_cpu;
    }

    if (
        !thread->context ||
        thread_get_state(thread) != THREAD_READY ||
        thread->pid == 0
    ) {
        return;
    }

    if (thread->on_rq && thread->last_cpu != cpu_id) {
        (void)rq_remove_thread(thread);
    }

    if (thread_cpu(thread) >= 0) {
        if (
            thread_is_owned(thread) ||
            thread_get_state(thread) == THREAD_RUNNING
        ) {
            return;
        }

        // recover stale running_cpu metadata only for non running states
        thread_set_cpu(thread, -1);
    }

    sched_rq_t *rq = &sched_state.runqueues[cpu_id];
    unsigned long flags = spin_lock_irqsave(&rq->lock);

    if (thread->on_rq || thread->rq_index != UINT32_MAX) {
        if (
            thread->last_cpu == cpu_id && thread->rq_index < rq->nr_running &&
            rq->heap[thread->rq_index] == thread
        ) {
            spin_unlock_irqrestore(&rq->lock, flags);
            return;
        }

        int found = rq_find_index(rq, thread);

        if (found >= 0) {
            thread->rq_index = (u32)found;
            thread->on_rq = true;
            thread->in_run_queue = true;
            thread->last_cpu = cpu_id;
            spin_unlock_irqrestore(&rq->lock, flags);
            return;
        }

        thread->on_rq = false;
        thread->in_run_queue = false;
        thread->rq_index = UINT32_MAX;
    }

    if (!thread->user_thread && thread->vruntime_ns < rq->min_vruntime) {
        thread->vruntime_ns = rq->min_vruntime;
    }

    if (!rq_insert(rq, thread)) {
        spin_unlock_irqrestore(&rq->lock, flags);
        return;
    }

    thread->on_rq = true;
    thread->in_run_queue = true;
    thread->last_cpu = cpu_id;
    thread->affinity_core = cpu_id;

    if (rq->nr_running > 0 && rq->heap[0]) {
        rq->min_vruntime = rq->heap[0]->vruntime_ns;
    }

    size_t depth = rq->nr_running;
    spin_unlock_irqrestore(&rq->lock, flags);

    rq_note_depth(depth);
}

static bool rq_remove_cpu(sched_rq_t *rq, sched_thread_t *thread) {
    if (!rq || !thread || !thread->on_rq) {
        return false;
    }

    if (!rq->heap) {
        thread->on_rq = false;
        thread->in_run_queue = false;
        thread->rq_index = UINT32_MAX;
        return false;
    }

    if (
        thread->rq_index == UINT32_MAX || thread->rq_index >= rq->nr_running ||
        rq->heap[thread->rq_index] != thread
    ) {
        int found = rq_find_index(rq, thread);

        if (found < 0) {
            thread->on_rq = false;
            thread->in_run_queue = false;
            thread->rq_index = UINT32_MAX;

            return false;
        }

        thread->rq_index = (u32)found;
    }

    return rq_remove_index(rq, thread->rq_index);
}

bool rq_remove_thread(sched_thread_t *thread) {
    if (!thread || !thread->on_rq) {
        return false;
    }

    sched_rq_t *rq = &sched_state.runqueues[thread->last_cpu];

    unsigned long flags = spin_lock_irqsave(&rq->lock);
    bool removed = rq_remove_cpu(rq, thread);
    spin_unlock_irqrestore(&rq->lock, flags);

    return removed;
}

sched_thread_t *rq_pop_best_allowed(size_t cpu_id) {
    if (cpu_id >= MAX_CORES) {
        return NULL;
    }

    sched_rq_t *rq = &sched_state.runqueues[cpu_id];
    unsigned long flags = spin_lock_irqsave(&rq->lock);

    if (rq->nr_running) {
        sched_thread_t *root = rq->heap[0];

        if (root && sched_cpu_allowed(root, cpu_id)) {
            rq_remove_index(rq, 0);
            spin_unlock_irqrestore(&rq->lock, flags);

            return root;
        }
    }

    u32 best_index = UINT32_MAX;
    sched_thread_t *best = NULL;

    for (u32 i = 0; (size_t)i < rq->nr_running; i++) {
        sched_thread_t *thread = rq->heap[i];

        if (!thread || !sched_cpu_allowed(thread, cpu_id)) {
            continue;
        }

        if (!best || rq_less(thread, best)) {
            best = thread;
            best_index = i;
        }
    }

    if (best && best_index != UINT32_MAX) {
        rq_remove_index(rq, best_index);
        spin_unlock_irqrestore(&rq->lock, flags);

        return best;
    }

    spin_unlock_irqrestore(&rq->lock, flags);
    return NULL;
}

sched_thread_t *rq_peek_best(size_t cpu_id) {
    if (cpu_id >= MAX_CORES) {
        return NULL;
    }

    sched_rq_t *rq = &sched_state.runqueues[cpu_id];
    unsigned long flags = spin_lock_irqsave(&rq->lock);
    sched_thread_t *thread = NULL;

    if (rq->nr_running) {
        sched_thread_t *root = rq->heap[0];

        if (root && sched_cpu_allowed(root, cpu_id)) {
            spin_unlock_irqrestore(&rq->lock, flags);
            return root;
        }
    }

    for (u32 i = 0; (size_t)i < rq->nr_running; i++) {
        sched_thread_t *candidate = rq->heap[i];

        if (!candidate || !sched_cpu_allowed(candidate, cpu_id)) {
            continue;
        }

        if (!thread || rq_less(candidate, thread)) {
            thread = candidate;
        }
    }

    spin_unlock_irqrestore(&rq->lock, flags);

    return thread;
}

sched_thread_t *
rq_pop_worst_allowed_from_cpu(size_t source_cpu, size_t target_cpu) {
    if (source_cpu >= MAX_CORES || target_cpu >= MAX_CORES) {
        return NULL;
    }

    sched_rq_t *rq = &sched_state.runqueues[source_cpu];
    unsigned long flags = spin_lock_irqsave(&rq->lock);

    sched_thread_t *candidate = NULL;
    u32 candidate_index = UINT32_MAX;

    for (u32 i = 0; (size_t)i < rq->nr_running; i++) {
        sched_thread_t *thread = rq->heap[i];

        if (
            !thread || thread_get_state(thread) != THREAD_READY ||
            !thread->context || thread->pid == 0 ||
            !sched_cpu_allowed(thread, target_cpu)
        ) {
            continue;
        }

        if (
            !candidate || thread->vruntime_ns > candidate->vruntime_ns ||
            (thread->vruntime_ns == candidate->vruntime_ns && thread->pid > candidate->pid)
        ) {
            candidate = thread;
            candidate_index = i;
        }
    }

    if (candidate && candidate_index != UINT32_MAX) {
        rq_remove_index(rq, candidate_index);
    } else {
        candidate = NULL;
    }

    spin_unlock_irqrestore(&rq->lock, flags);

    return candidate;
}

sched_thread_t *
rq_pop_disallowed_from_cpu(size_t source_cpu, size_t disallowed_cpu) {
    if (source_cpu >= MAX_CORES || disallowed_cpu >= MAX_CORES) {
        return NULL;
    }

    sched_rq_t *rq = &sched_state.runqueues[source_cpu];
    unsigned long flags = spin_lock_irqsave(&rq->lock);

    sched_thread_t *candidate = NULL;
    u32 candidate_index = UINT32_MAX;

    for (u32 i = 0; (size_t)i < rq->nr_running; i++) {
        sched_thread_t *thread = rq->heap[i];

        if (
            !thread || thread_get_state(thread) != THREAD_READY ||
            !thread->context || thread->pid == 0 ||
            sched_cpu_allowed(thread, disallowed_cpu)
        ) {
            continue;
        }

        if (
            !candidate || thread->vruntime_ns > candidate->vruntime_ns ||
            (thread->vruntime_ns == candidate->vruntime_ns && thread->pid > candidate->pid)
        ) {
            candidate = thread;
            candidate_index = i;
        }
    }

    if (candidate && candidate_index != UINT32_MAX) {
        rq_remove_index(rq, candidate_index);
    } else {
        candidate = NULL;
    }

    spin_unlock_irqrestore(&rq->lock, flags);
    return candidate;
}
