#include "lock.h"

#include <log/log.h>
#include <sched/scheduler.h>
#include <stdlib.h>

volatile uint32_t lock_spin_held_depth[MAX_CORES] = {0};

static sched_wait_queue_t *mutex_wait_queue_get(mutex_t *mutex, bool create) {
    if (!mutex) {
        return NULL;
    }

    sched_wait_queue_t *queue = __atomic_load_n(&mutex->wait_queue, __ATOMIC_ACQUIRE);
    if (queue || !create) {
        return queue;
    }

    queue = calloc(1, sizeof(*queue));
    if (!queue) {
        return NULL;
    }

    sched_wait_queue_init(queue);
    if (mutex->name) {
        sched_wait_queue_set_name(queue, mutex->name);
    }

    sched_wait_queue_t *expected = NULL;
    if (__atomic_compare_exchange_n(
            &mutex->wait_queue,
            &expected,
            queue,
            false,
            __ATOMIC_RELEASE,
            __ATOMIC_ACQUIRE
        )) {
        return queue;
    }

    sched_wait_queue_destroy(queue);
    free(queue);
    return expected;
}

void lock_preempt_disable(void) {
    if (!sched_is_running() || !sched_current()) {
        return;
    }

    sched_preempt_disable();
}

void lock_preempt_enable(void) {
    if (!sched_is_running() || !sched_current()) {
        return;
    }

    sched_preempt_enable();
}

void mutex_init(mutex_t *mutex) {
    if (!mutex) {
        return;
    }

    spinlock_init(&mutex->lock);
    mutex->held = 0;
    __atomic_store_n(&mutex->wait_queue, NULL, __ATOMIC_RELEASE);
    mutex->name = NULL;
#if LOCK_DEBUG
    mutex->owner_cpu = (size_t)-1;
#endif
}

void mutex_set_name(mutex_t *mutex, const char *name) {
    if (!mutex) {
        return;
    }

    mutex->name = name;
}

bool mutex_try_lock(mutex_t *mutex) {
    if (!mutex) {
        return false;
    }

    unsigned long flags = spin_lock_irqsave(&mutex->lock);
    bool ok = false;
    if (!mutex->held) {
        mutex->held = 1;
#if LOCK_DEBUG
        mutex->owner_cpu = lock_cpu_id();
#endif
        ok = true;
    }
    spin_unlock_irqrestore(&mutex->lock, flags);
    return ok;
}

void mutex_lock(mutex_t *mutex) {
    if (!mutex) {
        return;
    }

    sched_wait_queue_t *queue = mutex_wait_queue_get(mutex, true);
    for (;;) {
        unsigned long flags = spin_lock_irqsave(&mutex->lock);
        if (!mutex->held) {
            mutex->held = 1;
#if LOCK_DEBUG
            mutex->owner_cpu = lock_cpu_id();
#endif
            spin_unlock_irqrestore(&mutex->lock, flags);
            return;
        }
        u32 wait_seq = queue
                           ? __atomic_load_n(&queue->wake_seq, __ATOMIC_ACQUIRE)
                           : 0;
        spin_unlock_irqrestore(&mutex->lock, flags);

        if (!sched_is_running() || !sched_current() || !arch_irq_enabled()) {
            arch_cpu_relax();
            continue;
        }

        if (!queue) {
            queue = mutex_wait_queue_get(mutex, true);
            if (!queue) {
                arch_cpu_relax();
            }
            continue;
        }

        if (lock_spin_held_on_cpu()) {
            sched_lockdep_note_block_under_spin();
#if LOCK_DEBUG
            __builtin_trap();
#else
            arch_cpu_relax();
#endif
            continue;
        }

        if (!sched_preempt_disabled()) {
            (void)sched_wait_on_queue(queue, wait_seq, 0, 0);
        } else {
            sched_lockdep_note_block_under_spin();
            arch_cpu_relax();
        }
    }
}

void mutex_unlock(mutex_t *mutex) {
    if (!mutex) {
        return;
    }

    sched_wait_queue_t *queue = mutex_wait_queue_get(mutex, false);
    unsigned long flags = spin_lock_irqsave(&mutex->lock);
#if LOCK_DEBUG
    if (!mutex->held || mutex->owner_cpu != lock_cpu_id()) {
        spin_unlock_irqrestore(&mutex->lock, flags);
        __builtin_trap();
    }
#endif
    mutex->held = 0;
#if LOCK_DEBUG
    mutex->owner_cpu = (size_t)-1;
#endif
    spin_unlock_irqrestore(&mutex->lock, flags);

    if (queue && sched_is_running()) {
        sched_wake_one(queue);
    }
}
