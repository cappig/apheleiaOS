#pragma once

#include <arch/arch.h>

static inline void lock(volatile int* state) {
    if (!state)
        return;

    while (__sync_lock_test_and_set(state, 1)) {
        while (*state)
            arch_cpu_wait();
    }
}

static inline void unlock(volatile int* state) {
    if (!state)
        return;

    __sync_lock_release(state);
}
