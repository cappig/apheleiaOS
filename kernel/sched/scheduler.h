#pragma once

#include <base/attributes.h>
#include <base/types.h>

#include "arch/idt.h"
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

extern tree* proc_tree;


process* process_with_pid(usize pid);

bool process_validate_ptr(process* proc, const void* ptr, usize len, bool write);

void schedule(void);
void scheduler_tick(void);

void scheduler_save(int_state* s);
void scheduler_switch(void) NORETURN;

void scheduler_queue(process* proc);
void scheduler_sleep(process* proc, usize milis);
void scheduler_kill(process* proc, usize status);

void dump_process_tree(void);

void scheduler_init(void);
void scheduler_start(void) NORETURN;
