#pragma once

#include <data/vector.h>

#include "sched/process.h"

// For blocking I/O. Slow devices should store a list of waiting processes.
// When the data is ready all process in the list are woken up and
// made ready for scheduling again
typedef struct {
    vector* procs;
    // lock?
} wait_list;


wait_list* wait_list_create(void);
void wait_list_destroy(wait_list* list);

bool wait_list_append(wait_list* list, process* proc);
bool wait_list_wake_up(wait_list* list);
