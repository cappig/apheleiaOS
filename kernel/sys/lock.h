#pragma once

#include <arch/arch.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/cpu.h>

struct sched_wait_queue;
extern volatile uint32_t lock_spin_held_depth[MAX_CORES];

typedef struct {
    volatile int state;
#if LOCK_DEBUG
    size_t owner_cpu;
#endif
} spinlock_t;

typedef struct {
    spinlock_t lock;
    volatile int held;
    struct sched_wait_queue *wait_queue;
#if LOCK_DEBUG
    size_t owner_cpu;
#endif
} mutex_t;

void lock_preempt_disable(void);
void lock_preempt_enable(void);

#define SPINLOCK_INIT \
    { \
        .state = 0 \
    }

#define MUTEX_INIT \
    { \
        .lock = SPINLOCK_INIT, \
        .held = 0, \
        .wait_queue = NULL \
    }

static inline size_t lock_cpu_id(void) {
    cpu_core_t *core = cpu_current();

    if (!core || core->id >= MAX_CORES) {
        return 0;
    }

    return core->id;
}

static inline void spinlock_init(spinlock_t *lock) {
    if (!lock) {
        return;
    }

    lock->state = 0;
#if LOCK_DEBUG
    lock->owner_cpu = (size_t)-1;
#endif
}

static inline bool spin_try_lock(spinlock_t *lock) {
    if (!lock) {
        return false;
    }

    lock_preempt_disable();

    if (__sync_lock_test_and_set(&lock->state, 1)) {
        lock_preempt_enable();
        return false;
    }

    size_t cpu_id = lock_cpu_id();

#if LOCK_DEBUG
    lock->owner_cpu = cpu_id;
#endif

    __atomic_fetch_add(
        &lock_spin_held_depth[cpu_id], 1U, __ATOMIC_RELAXED
    );

    return true;
}

static inline void spin_lock(spinlock_t *lock) {
    if (!lock) {
        return;
    }

    lock_preempt_disable();

    size_t cpu_id = lock_cpu_id();

#if LOCK_DEBUG
    if (lock->owner_cpu == cpu_id) {
        __builtin_trap();
    }
#endif

    while (__sync_lock_test_and_set(&lock->state, 1)) {
        while (__atomic_load_n(&lock->state, __ATOMIC_RELAXED)) {
            arch_cpu_relax();
        }
    }

#if LOCK_DEBUG
    lock->owner_cpu = cpu_id;
#endif
    __atomic_fetch_add(
        &lock_spin_held_depth[cpu_id], 1U, __ATOMIC_RELAXED
    );
}

static inline void spin_unlock(spinlock_t *lock) {
    if (!lock) {
        return;
    }

    size_t cpu_id = lock_cpu_id();

#if LOCK_DEBUG
    if (lock->owner_cpu != cpu_id) {
        __builtin_trap();
    }

    lock->owner_cpu = (size_t)-1;
#endif

    uint32_t depth_prev =
        __atomic_fetch_sub(&lock_spin_held_depth[cpu_id], 1U, __ATOMIC_RELAXED);

    if (!depth_prev) {
        __atomic_fetch_add(
            &lock_spin_held_depth[cpu_id], 1U, __ATOMIC_RELAXED
        );

#if LOCK_DEBUG
        __builtin_trap();
#endif
    }

    __sync_lock_release(&lock->state);
    lock_preempt_enable();
}

static inline bool lock_spin_held_on_cpu(void) {
    size_t cpu_id = lock_cpu_id();
    return __atomic_load_n(&lock_spin_held_depth[cpu_id], __ATOMIC_RELAXED) != 0;
}

static inline unsigned long spin_lock_irqsave(spinlock_t *lock) {
    unsigned long flags = arch_irq_save();
    spin_lock(lock);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags) {
    spin_unlock(lock);
    arch_irq_restore(flags);
}

void mutex_init(mutex_t *mutex);
bool mutex_try_lock(mutex_t *mutex);
void mutex_lock(mutex_t *mutex);
void mutex_unlock(mutex_t *mutex);
