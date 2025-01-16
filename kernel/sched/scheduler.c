#include "scheduler.h"

#include <aos/signals.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <data/list.h>
#include <log/log.h>
#include <parse/elf.h>
#include <x86/asm.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "arch/irq.h"
#include "data/tree.h"
#include "drivers/initrd.h"
#include "mem/heap.h"
#include "sched/process.h"
#include "sched/signal.h"
#include "sched/syscall.h"
#include "sys/clock.h"
#include "sys/panic.h"
#include "sys/tty.h"

// Defined in switch.asm
extern NORETURN void context_switch(u64 kernel_stack);

// TODO: this should be per core
scheduler sched_instance = {0};

static tree* proc_tree;


// This kernel pseudo-process is scheduled if there are no real process left to run
// It just halts the machine and waits for the next time slice
static void _spin(void) {
    for (;;)
        halt();
}


static bool _proc_comp(const void* data, const void* private) {
    process* proc = (process*)data;
    usize pid = (usize) private;

    return (proc->id == pid);
}

process* process_with_pid(usize pid) {
    assert(sched_instance.running);

    tree_node* res = tree_find_comp(proc_tree, _proc_comp, (void*)pid);

    if (!res)
        return NULL;

    return res->data;
}


// Check if a sleeping process has to be woken up
static bool _wake_sleeper(void) {
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

    if (!sleeper_to_wake)
        return false;

    sleeping_process* sproc = sleeper_to_wake->data;
    process* proc = sproc->proc;

    kfree(sproc);

    list_remove(sched_instance.sleep_queue, sleeper_to_wake);
    list_destroy_node(sleeper_to_wake);

    proc->state = PROC_RUNNING;

    sched_instance.current = proc;

#ifdef SCHED_DEBUG
    log_debug("[SCHED_DEBUG] waking up sleeping process: pid=%lu", proc->id);
#endif

    return true;
}

// Fetch the process that should get the next time slice
static void _get_next_process(scheduler* sched) {
    bool found = false;

    foreach (node, sched->run_queue) {
        process* proc = node->data;

        // Ignore blocked and finished processes
        if (proc->state == PROC_READY || proc->state == PROC_RUNNING) {
            list_node* process_node = list_pop_front(sched->run_queue);
            list_append(sched->run_queue, process_node);

            sched->current = proc;
            found = true;

            break;
        }
    }

    // There are no processes left to run so we schedule idle
    if (!found)
        sched->current = sched->idle;
}


void scheduler_tick() {
    sched_instance.proc_ticks_left--;

    foreach (node, sched_instance.sleep_queue) {
        sleeping_process* proc = node->data;
        proc->time_left--;
    }
}

// Figure out which process should get run on the next context switch
void schedule() {
    bool sleeper = _wake_sleeper();

    // Is the current timeslice is done
    if (sched_instance.proc_ticks_left <= 0 && !sleeper) {
        _get_next_process(&sched_instance);
        sched_instance.proc_ticks_left = SCHED_SLICE;
    }

    // Are there any pending signals
    if (sched_instance.current->type == PROC_USER) {
        usize signum = signal_get_pending(sched_instance.current);
        prepare_signal(sched_instance.current, signum);
    }

#ifdef SCHED_DEBUG
    log_debug(
        "[SCHED_DEBUG] scheduling process: name=%s pid=%lu",
        sched_instance.current->name,
        sched_instance.current->id
    );
#endif
}

NORETURN
void scheduler_switch() {
    assert(sched_instance.current);

    // The process will have valid state to save after this time slice
    if (sched_instance.current->state == PROC_READY)
        sched_instance.current->state = PROC_RUNNING;

    u64 ksp = (u64)sched_instance.current->stack_ptr;

    // Kernel processes don't have to switch the page table
    // Userspace processes must set the TSS to be able to switch back to ring 0
    if (sched_instance.current->type == PROC_USER) {
        write_cr3((u64)sched_instance.current->user.mem_map);
        set_tss_stack(ksp + sizeof(int_state));
    }

    context_switch(ksp);
    __builtin_unreachable();
}


// Since kernel processes use a single stack the stack pointer has
// to be updated once we context switch to a different process
// This is done automatically for user processes due to the TSS
void scheduler_save(int_state* s) {
    if (!s)
        return;

    if (sched_instance.current->type != PROC_KERNEL)
        return;

    sched_instance.current->stack_ptr = (u64)s;
}


void scheduler_queue(process* proc) {
    list_node* node = list_create_node(proc);
    list_append(sched_instance.run_queue, node);
}

// NOTE: the sleeping process _must_ be in the run queue as well
void scheduler_sleep(process* proc, usize milis) {
    proc->state = PROC_BLOCKED;

    sleeping_process* sproc = kcalloc(sizeof(sleeping_process));

    sproc->proc = proc;
    sproc->time_left = DIV_ROUND_UP(milis, MS_PER_TICK);

    list_node* node = list_create_node(sproc);
    list_append(sched_instance.sleep_queue, node);
}


void scheduler_kill(process* proc, usize status) {
    list_node* node = list_find(sched_instance.run_queue, proc);

    assert(node);

    list_remove(sched_instance.run_queue, node);
    list_destroy_node(node);

    process_free(proc);

    proc->status = status;

    // Pass the unfortunate news to the parent
    tree_node* parent = proc->user.tree_entry->parent;

#ifdef SCHED_DEBUG
    log_debug("[SCHED_DEBUG] killing process: name=%s pid=%lu", proc->name, proc->id);
#endif

    if (!parent)
        panic("Attempted to kill init (pid = %zu)!", proc->id);

    signal_send(parent->data, SIGCHLD);

    sched_instance.proc_ticks_left = 0;
    schedule();
}


// Spawn the init process (PID 1)
static void _spawn_init(void) {
    process* init = spawn_uproc("init");

    ustar_file* elf = initrd_find("usr/init.elf");

    if (!elf)
        panic("init.elf not found!");

    bool exec = process_exec_elf(init, (elf_header*)elf->data);

    if (!exec)
        panic("Failed to start init");

    proc_tree = tree_create(init);
    init->user.tree_entry = proc_tree->root;

    scheduler_queue(init);
}

void scheduler_init() {
    sched_instance.run_queue = list_create();
    sched_instance.sleep_queue = list_create();

    sched_instance.idle = spawn_kproc("[idle]", _spin);

    load_vdso();
    syscall_init();

    _spawn_init();
}


NORETURN
void scheduler_start() {
    log_info("Starting the scheduler");

    tty_set_current(0);

    // Bootstrap the scheduler
    schedule();

    timer_enable();

    sched_instance.running = true;

    scheduler_switch();
    __builtin_unreachable();
}
