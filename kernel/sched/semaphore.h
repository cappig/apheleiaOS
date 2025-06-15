#pragma once

#include <data/list.h>
#include <stdatomic.h>

#include "arch/lock.h"

#define SEM_PASSABLE     1
#define SEM_NOT_PASSABLE 0

typedef struct {
    atomic_bool passable;
    lock spin_lock;

    linked_list wait_list;
} semaphore;


semaphore* sem_create(bool initial_state);
void sem_destroy(semaphore* sem);

int sem_wait(semaphore* sem, bool wait);

void sem_signal(semaphore* sem);
void sem_reset(semaphore* sem);
