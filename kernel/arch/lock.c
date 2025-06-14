#include "lock.h"


void spin_lock(lock* l) {
    while (__atomic_test_and_set(l, __ATOMIC_ACQUIRE))
        asm volatile("pause");
}

void spin_unlock(lock* l) {
    __atomic_clear(l, __ATOMIC_RELEASE);
}
