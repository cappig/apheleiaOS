#pragma once

#include <stdbool.h>

#include "base/attributes.h"
#include "base/types.h"
#include "data/list.h"
#include "process.h"

#define SCHED_SLICE 1

#define INIT_PID 1

typedef struct {
    bool running;

    isize ticks_left;
    bool needs_resched;

    linked_list* run_queue;
    sched_thread* current;
} core_scheduler;

// defined in switch.asm
extern void context_switch(u64 kernel_stack) NORETURN;


void sched_enqueue(sched_thread* thread);
bool sched_dequeue(sched_thread* thread, bool and_current);
void sched_enqueue_proc(sched_process* proc);
bool sched_dequeue_proc(sched_process* proc);

void sched_thread_sleep(sched_thread* thread, u64 milis);

sched_process* sched_get_proc(pid_t pid);

void sched_switch(void) NORETURN;
void sched_save(int_state* s);

void scheduler_init(void);
void scheduler_start(void) NORETURN;

void schedule(void);

void dump_process_tree(void);
