#include "scheduler.h"

#include <base/attributes.h>
#include <base/types.h>
#include <data/list.h>
#include <log/log.h>
#include <x86/asm.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "mem/heap.h"
#include "sched/process.h"
#include "sys/clock.h"

// Defined in switch.asm
extern NORETURN void context_switch(u64 kernel_stack);

// TODO: this should be per core
scheduler sched_instance = {0};

// This kernel pseudo-process is scheduled if there are no real process left to run
// It just halts the machine and waits for the next time slice
static void _spin(void) {
    for (;;)
        halt();
}


// Fetch the process that should get the next time slice
static void get_next_process(scheduler* sched) {
    // There are no processes to run so we schedule idle
    if (!sched->run_queue->length) {
        sched->current = sched->idle;
        return;
    }

    // Ignore blocked and finished processes
    do {
        list_node* process_node = list_pop_front(sched->run_queue);
        sched->current = process_node->data;
        list_append(sched->run_queue, process_node);
    } while (sched->current->state != PROC_READY && sched->current->state != PROC_RUNNING);
}

// Update the old ksp if the current_process is running
void scheduler_save(int_state* s) {
    if (sched_instance.current->state == PROC_RUNNING && s != NULL)
        sched_instance.current->ksp = (u8*)s;
}

// Figure out which process should get run on the next context switch
void schedule() {
    if (!sched_instance.running)
        return;

    // Check if there are any sleeping tasks to wake up
    // The times have been recalculated by the system clock
    list_node* sleeper_to_wake = NULL;
    isize longest_wait = 1;

    foreach (node, sched_instance.sleep_queue) {
        sleeping_process* curr = node->data;

        // Find the process that has been waiting the longest
        // Prefer processes higher up in the queue
        if (curr->time_left <= 0 && curr->time_left < longest_wait) {
            longest_wait = curr->time_left;

            sleeper_to_wake = node;
            sched_instance.current = curr->proc;
        }
    }

    if (sleeper_to_wake) {
        sleeping_process* sproc = sleeper_to_wake->data;
        process* proc = sproc->proc;

        kfree(sproc);

        list_remove(sched_instance.sleep_queue, sleeper_to_wake);
        list_destroy_node(sleeper_to_wake);

        proc->state = PROC_RUNNING;

#ifdef SCHED_DEBUG
        log_debug("[SCHED_DEBUG] waking up process: pid=%lu", proc->id);
#endif
    }

    // If there are none check if the current timeslice is done
    if (sched_instance.proc_ticks_left <= 0 && !sleeper_to_wake) {
        get_next_process(&sched_instance);
        sched_instance.proc_ticks_left = SCHED_SLICE;
    }

#ifdef SCHED_DEBUG
    log_debug(
        "[SCHED_DEBUG] scheduling process: name=%s pid=%lu ksp=%#lx cr3=%#lx",
        sched_instance.current->name,
        sched_instance.current->id,
        (u64)sched_instance.current->ksp,
        (u64)sched_instance.current->cr3
    );
#endif
}

NORETURN
void scheduler_switch() {
    // The process will have valid state to save after this time slice
    if (sched_instance.current->state == PROC_READY)
        sched_instance.current->state = PROC_RUNNING;

#ifdef SCHED_DEBUG
    log_debug(
        "[SCHED_DEBUG] preparing context switch to pid = %lu, ksp = %#lx",
        sched_instance.current->id,
        (u64)sched_instance.current->ksp
    );
#endif

    // Kernel processes don't have to switch the page table
    // Userspace processes must set the TSS to be able to switch back to ring 0
    if (sched_instance.current->type == PROC_USER) {
        u64 ksp = (u64)sched_instance.current->kernel_stack + SCHED_KSTAK_SIZE;
        set_tss_stack(ksp);

        write_cr3((u64)sched_instance.current->cr3);
    }

    context_switch((u64)sched_instance.current->ksp);
    __builtin_unreachable();
}


void scheduler_queue(process* proc) {
    list_node* node = list_create_node(proc);
    list_append(sched_instance.run_queue, node);
}

void scheduler_kill(process* proc) {
    list_node* node = list_find(sched_instance.run_queue, proc);

    if (!node)
        return;

    list_remove(sched_instance.run_queue, node);
    list_destroy_node(node);
}

void scheduler_sleep(process* proc, usize milis) {
    proc->state = PROC_BLOCKED;

    sleeping_process* sproc = kcalloc(sizeof(sleeping_process));
    sproc->proc = proc;
    sproc->time_left = milis / MS_PER_TICK;

    list_node* node = list_create_node(sproc);
    list_append(sched_instance.sleep_queue, node);
}


void scheduler_tick() {
    sched_instance.proc_ticks_left--;

    // FIXME: this is retarded!
    foreach (node, sched_instance.sleep_queue) {
        sleeping_process* curr = node->data;
        curr->time_left--;
    }

    schedule();
}

void scheduler_exit(int status) {
    scheduler_kill(sched_instance.current);

    sched_instance.proc_ticks_left = 0;
    schedule();
}


void scheduler_init() {
    sched_instance.run_queue = list_create();
    sched_instance.sleep_queue = list_create();

    sched_instance.idle = spawn_kproc("[idle]", _spin);
}

NORETURN
void scheduler_start() {
    log_info("Starting the scheduler");

    // Bootstrap the scheduler
    sched_instance.running = true;

    get_next_process(&sched_instance);

    scheduler_switch();
    __builtin_unreachable();
}
