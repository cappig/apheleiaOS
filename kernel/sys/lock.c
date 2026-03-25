#include "lock.h"

#include <arch/arch.h>
#include <inttypes.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/cpu.h>
#include <sys/panic.h>
#if defined(__x86_64__) || defined(__i386__)
#include <x86/serial.h>
#endif

volatile uint32_t lock_spin_held_depth[MAX_CORES] = {0};

#if LOCK_DEBUG
void lock_debug_trap(
    const char *site,
    const void *lock_ptr,
    const void *caller,
    size_t owner_cpu,
    int lock_state
) {
    size_t cpu_id = lock_cpu_id();
    sched_thread_t *current = sched_is_running() ? sched_current() : NULL;
    char buf[LOG_BUF_SIZE] = {0};
    int len = snprintf(
        buf,
        sizeof(buf),
        "fatal %s:%d lock debug trap site=%s lock=%#" PRIx64
        " caller=%#" PRIx64 " owner_cpu=%zu cpu=%zu state=%d held_depth=%u"
        " current=%#" PRIx64 " pid=%d name=%s\n",
        __FILE__,
        __LINE__,
        site ? site : "?",
        (u64)(uintptr_t)lock_ptr,
        (u64)(uintptr_t)caller,
        owner_cpu,
        cpu_id,
        lock_state,
        (unsigned)__atomic_load_n(&lock_spin_held_depth[cpu_id], __ATOMIC_RELAXED),
        (u64)(uintptr_t)current,
        current ? current->pid : 0,
        current ? current->name : "none"
    );

#if defined(__x86_64__) || defined(__i386__)
    if (len > 0) {
        size_t size = (size_t)len;
        if (size > sizeof(buf)) {
            size = sizeof(buf);
        }
        send_serial_sized_string(SERIAL_COM1, buf, size);
    }
#endif

    (void)arch_irq_save();
    cpu_halt();
}
#endif

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

#if LOCK_DEBUG
    mutex->owner_thread = NULL;
#endif
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
        mutex->owner_thread = sched_is_running() ? sched_current() : NULL;
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
            mutex->owner_thread = sched_is_running() ? sched_current() : NULL;
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
#if LOCK_DEBUG
            lock_debug_trap(
                "mutex_lock:spin-depth-held",
                mutex,
                __builtin_return_address(0),
                (size_t)-1,
                mutex->held
            );
#else
            arch_cpu_relax();
#endif
            continue;
        }

        if (!sched_preempt_disabled()) {
            (void)sched_wait_on_queue(queue, wait_seq, 0, 0);
        } else {
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
    if (!mutex->held) {
        spin_unlock_irqrestore(&mutex->lock, flags);
        lock_debug_trap(
            "mutex_unlock:not-held",
            mutex,
            __builtin_return_address(0),
            (size_t)-1,
            mutex->held
        );
    }

    if (sched_is_running()) {
        sched_thread_t *current = sched_current();

        if (current && mutex->owner_thread != current) {
            spin_unlock_irqrestore(&mutex->lock, flags);
            lock_debug_trap(
                "mutex_unlock:foreign-owner",
                mutex,
                __builtin_return_address(0),
                (size_t)-1,
                mutex->held
            );
        }
    }
#endif
    mutex->held = 0;
#if LOCK_DEBUG
    mutex->owner_thread = NULL;
#endif

    spin_unlock_irqrestore(&mutex->lock, flags);

    if (queue && sched_is_running()) {
        sched_wake_one(queue);
    }
}
