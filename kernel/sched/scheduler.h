#pragma once

#include <base/types.h>

#include "sched/process.h"

// Each process will run for at most SCHED_SLICE ticks
#define SCHED_SLICE 1

typedef struct {
    isize time_left;
    process* proc;
} sleeping_process;

// A single instance of the scheduler. Each CPU has one
typedef struct {
    bool running;

    // How many ticks does the current processes time slice last
    isize proc_ticks_left;

    linked_list* run_queue;
    linked_list* sleep_queue;

    process* current;
    process* idle;
} scheduler;

// TODO: make this per core!
extern scheduler sched_instance;


void scheduler_init(void);
NORETURN void scheduler_start(void);

void schedule();
void scheduler_save(int_state* s);

NORETURN void scheduler_switch(void);

void scheduler_tick(void);

void scheduler_queue(process* proc);
void scheduler_kill(process* proc);

void scheduler_exit(int status);

void scheduler_sleep(process* proc, usize milis);
