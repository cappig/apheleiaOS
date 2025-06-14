#pragma once

#include <data/list.h>
#include <stdatomic.h>

#include "arch/lock.h"
#include "sched/process.h"

// Blocking I/O devices should store a list of waiting processes.
// When the data is ready all process in the list are woken up and
// made ready for scheduling again
typedef struct {
    atomic_bool passable;
    lock spinlock;

    linked_list list;
} wait_list;


wait_list* wait_list_create(void);
void wait_list_destroy(wait_list* list);

bool wait_list_append(wait_list* list, sched_thread* thread);
bool wait_list_wake_up(wait_list* list);
