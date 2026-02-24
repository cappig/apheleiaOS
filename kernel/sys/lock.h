#pragma once

#include <arch/arch.h>

static inline void lock(volatile int *state) {
    if (!state)
        return;

    while (__sync_lock_test_and_set(state, 1)) {
        while (*state)
            arch_cpu_wait();
    }
}

static inline void unlock(volatile int *state) {
    if (!state)
        return;

    __sync_lock_release(state);
}

static inline unsigned long lock_irqsave(volatile int *state) {
    unsigned long flags = arch_irq_save();
    lock(state);
    return flags;
}

static inline void unlock_irqrestore(volatile int *state, unsigned long flags) {
    unlock(state);
    arch_irq_restore(flags);
}
