#include "semaphore.h"

#include <errno.h>

#include "arch/lock.h"
#include "mem/heap.h"
#include "sched/signal.h"
#include "sys/cpu.h"


static void _wait_list_add(semaphore* sem, sched_thread* thread) {
    spin_lock(&sem->spin_lock);

    thread->state = T_SLEEPING;

    sched_dequeue(thread, true);
    list_append(&sem->wait_list, &thread->lnode);

    spin_unlock(&sem->spin_lock);
}

static void _wait_list_remove(semaphore* sem, sched_thread* thread) {
    spin_lock(&sem->spin_lock);

    list_remove(&sem->wait_list, &thread->lnode);
    sched_enqueue(thread);

    spin_unlock(&sem->spin_lock);
}


semaphore* sem_create(bool initial_state) {
    semaphore* sem = kcalloc(sizeof(semaphore));

    sem->passable = initial_state;
    sem->spin_lock = SPINLOCK_UNLOCKED;

    return sem;
}

void sem_destroy(semaphore* sem) {
    sem_signal(sem);
    list_destroy(&sem->wait_list, false);
}


int sem_wait(semaphore* sem, bool wait) {
    if (!sem)
        return -EINVAL;

    // is the semaphore passable wight away
    if (sem->passable)
        return 1;

    sched_thread* thread = cpu_current_thread();
    _wait_list_add(sem, thread);

    // not passable but we do not want to block (this function)
    if (!wait)
        return 0;

    for (;;) {
        // interrupted by a signal
        if (thread_signal_get_pending(thread)) {
            _wait_list_remove(sem, thread);
            return -EINTR;
        }

        // became passable
        if (sem->passable) {
            //_wait_list_remove(sem, thread);
            return 1;
        }

        // sched_yield();
    }
}

void sem_signal(semaphore* sem) {
    if (!sem)
        return;

    sem->passable = true;

    list_node* node = sem->wait_list.head;
    while (node) {
        sched_thread* thread = node->data;
        list_node* next = node->next;

        _wait_list_remove(sem, thread);

        node = next;
    }
}

void sem_reset(semaphore* sem) {
    if (!sem)
        return;

    sem->passable = SEM_NOT_PASSABLE;
}
