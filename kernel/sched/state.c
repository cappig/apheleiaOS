#include "internal.h"

sched_state_t sched_state = {
    .next_user_pid = 1,
    .next_kernel_pid = -1,
    .lock = SPINLOCK_INIT,
    .exit_events = {
        .lock = SPINLOCK_INIT,
    },
};

bool sched_reconcile_lock(void) {
    sched_cpu_state_t *local = sched_local();
    if (!local->sched_lock_depth) {
        return false;
    }

    if (__atomic_load_n(&sched_state.lock.state, __ATOMIC_RELAXED)) {
        return true;
    }

    local->sched_lock_depth = 0;
    local->sched_lock_irq_flags = 0;

    return false;
}

unsigned long sched_lock_save(void) {
    unsigned long flags = arch_irq_save();
    const unsigned long initial_flags = flags;

    sched_cpu_state_t *local = sched_local();

    sched_reconcile_lock();

    if (local->sched_lock_depth) {
        local->sched_lock_depth++;
        return 0;
    }

    for (;;) {
        if (spin_try_lock(&sched_state.lock)) {
            local = sched_local();
            local->sched_lock_depth = 1;
            local->sched_lock_irq_flags = initial_flags;

            break;
        }

        arch_irq_restore(flags);
        arch_cpu_relax();

        flags = arch_irq_save();
    }

    return initial_flags;
}

bool sched_lock_try_save(unsigned long *flags_out) {
    if (!flags_out) {
        return false;
    }

    unsigned long flags = arch_irq_save();

    sched_cpu_state_t *local = sched_local();
    sched_reconcile_lock();

    if (local->sched_lock_depth) {
        local->sched_lock_depth++;
        *flags_out = 0;
        return true;
    }

    if (!spin_try_lock(&sched_state.lock)) {
        arch_irq_restore(flags);
        return false;
    }

    local->sched_lock_depth = 1;
    local->sched_lock_irq_flags = flags;
    *flags_out = flags;

    return true;
}

void sched_lock_restore(unsigned long flags) {
    sched_cpu_state_t *local = sched_local();

    if (local->sched_lock_depth > 1) {
        local->sched_lock_depth--;
        return;
    }

    if (local->sched_lock_depth == 1) {
        unsigned long irq_flags = local->sched_lock_irq_flags;

        local->sched_lock_depth = 0;
        local->sched_lock_irq_flags = 0;
        spin_unlock(&sched_state.lock);

        arch_irq_restore(irq_flags);
        return;
    }

    arch_irq_restore(flags);
}

static bool sleep_heap_less(size_t left, size_t right) {
    sched_thread_t *a = sched_state.sleep.heap[left];
    sched_thread_t *b = sched_state.sleep.heap[right];

    if (!a) {
        return false;
    }

    if (!b) {
        return true;
    }

    if (a->wake_tick != b->wake_tick) {
        return a->wake_tick < b->wake_tick;
    }

    return a->pid < b->pid;
}

static void sleep_heap_swap(size_t left, size_t right) {
    sched_thread_t *tmp = sched_state.sleep.heap[left];

    sched_state.sleep.heap[left] = sched_state.sleep.heap[right];
    sched_state.sleep.heap[right] = tmp;

    if (sched_state.sleep.heap[left]) {
        sched_state.sleep.heap[left]->sleep_index = left;
    }

    if (sched_state.sleep.heap[right]) {
        sched_state.sleep.heap[right]->sleep_index = right;
    }
}

static void sleep_heap_sift_up(size_t index) {
    while (index > 0) {
        size_t parent = (index - 1) / 2;

        if (!sleep_heap_less(index, parent)) {
            break;
        }

        sleep_heap_swap(index, parent);
        index = parent;
    }
}

static void sleep_heap_sift_down(size_t index) {
    for (;;) {
        size_t left = index * 2 + 1;
        size_t right = left + 1;
        size_t best = index;

        if (left < sched_state.sleep.count && sleep_heap_less(left, best)) {
            best = left;
        }

        if (right < sched_state.sleep.count && sleep_heap_less(right, best)) {
            best = right;
        }

        if (best == index) {
            return;
        }

        sleep_heap_swap(index, best);
        index = best;
    }
}

static ssize_t sleep_heap_find_thread(const sched_thread_t *thread) {
    if (!thread) {
        return -1;
    }

    for (size_t i = 0; i < sched_state.sleep.count; i++) {
        if (sched_state.sleep.heap[i] == thread) {
            return (ssize_t)i;
        }
    }

    return -1;
}

bool sleep_heap_insert(sched_thread_t *thread) {
    if (!thread) {
        return true;
    }

    if (thread->sleep_queued) {
        if (
            thread->sleep_index < sched_state.sleep.count &&
            sched_state.sleep.heap[thread->sleep_index] == thread
        ) {
            sleep_heap_remove_at(thread->sleep_index);
        } else {
            ssize_t found = sleep_heap_find_thread(thread);

            if (found >= 0) {
                sleep_heap_remove_at((size_t)found);
            }
        }

        thread->sleep_queued = false;
        thread->sleep_index = 0;
    }

    ssize_t found = sleep_heap_find_thread(thread);
    if (found >= 0) {
        sleep_heap_remove_at((size_t)found);
    }

    if (sched_state.sleep.count >= SCHED_SLEEP_HEAP_CAPACITY) {
        return false;
    }

    size_t index = sched_state.sleep.count++;

    sched_state.sleep.heap[index] = thread;
    thread->sleep_queued = true;
    thread->sleep_index = index;
    sleep_heap_sift_up(index);

    return true;
}

void sleep_heap_remove_at(size_t index) {
    if (index >= sched_state.sleep.count) {
        return;
    }

    sched_thread_t *removed = sched_state.sleep.heap[index];
    sched_state.sleep.count--;

    if (index != sched_state.sleep.count) {
        sched_state.sleep.heap[index] = sched_state.sleep.heap[sched_state.sleep.count];

        if (sched_state.sleep.heap[index]) {
            sched_state.sleep.heap[index]->sleep_index = index;
        }

        sleep_heap_sift_down(index);
        sleep_heap_sift_up(index);
    }

    sched_state.sleep.heap[sched_state.sleep.count] = NULL;

    if (removed) {
        removed->sleep_queued = false;
        removed->sleep_index = 0;
    }
}

void sleep_heap_remove(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    if (
        thread->sleep_queued &&
        thread->sleep_index < sched_state.sleep.count &&
        sched_state.sleep.heap[thread->sleep_index] == thread
    ) {
        sleep_heap_remove_at(thread->sleep_index);
        return;
    }

    ssize_t found = sleep_heap_find_thread(thread);
    if (found >= 0) {
        thread->sleep_queued = true;
        thread->sleep_index = (size_t)found;
        sleep_heap_remove_at((size_t)found);
        return;
    }

    thread->sleep_queued = false;
    thread->sleep_index = 0;
}

sched_thread_t *sleep_heap_top(void) {
    if (!sched_state.sleep.count) {
        return NULL;
    }

    return sched_state.sleep.heap[0];
}

pid_t sched_next_pid(sched_pid_class_t pid_class) {
    unsigned long flags = sched_lock_save();
    pid_t pid = 0;

    switch (pid_class) {
    case SCHED_PID_IDLE:
        break;
    case SCHED_PID_USER:
        if (sched_state.next_user_pid <= 0 || sched_state.next_user_pid == INT_MAX) {
            panic("user PID space exhausted");
        }
        pid = sched_state.next_user_pid++;
        break;
    case SCHED_PID_KERNEL:
        if (sched_state.next_kernel_pid >= 0 || sched_state.next_kernel_pid == INT_MIN) {
            panic("kernel PID space exhausted");
        }
        pid = sched_state.next_kernel_pid--;
        break;
    default:
        panic("invalid scheduler PID class");
    }

    sched_lock_restore(flags);
    return pid;
}
