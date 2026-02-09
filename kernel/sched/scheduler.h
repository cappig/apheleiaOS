#pragma once

#include <arch/context.h>
#include <base/attributes.h>
#include <base/types.h>
#include <data/list.h>

typedef void (*thread_entry_t)(void* arg);

typedef enum {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_SLEEPING,
    THREAD_ZOMBIE,
} thread_state_t;

typedef struct sched_thread {
    const char* name;
    thread_state_t state;

    list_node_t run_node;
    bool in_run_queue;

    list_node_t wait_node;
    bool in_wait_queue;

    list_node_t zombie_node;
    bool in_zombie_list;

    bool is_bootstrap;

    void* stack;
    size_t stack_size;
    uintptr_t context;

    thread_entry_t entry;
    void* arg;
} sched_thread_t;

typedef struct sched_wait_queue {
    linked_list_t* list;
} sched_wait_queue_t;

void scheduler_init(void);
void scheduler_start(void);

bool sched_is_running(void);

sched_thread_t* sched_current(void);
sched_thread_t* sched_create_kernel_thread(const char* name, thread_entry_t entry, void* arg);

void sched_wait_queue_init(sched_wait_queue_t* queue);
void sched_wait_queue_destroy(sched_wait_queue_t* queue);

void sched_block(sched_wait_queue_t* queue);
void sched_block_locked(sched_wait_queue_t* queue, unsigned long flags);
void sched_wake_one(sched_wait_queue_t* queue);
void sched_wake_all(sched_wait_queue_t* queue);

void sched_tick(arch_int_state_t* state);
void sched_yield(void);
void sched_exit(void) NORETURN;
