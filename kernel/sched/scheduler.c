#include "scheduler.h"

#include <aos/syscalls.h>
#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>
#include <log/log.h>
#include <time.h>
#include <x86/asm.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "arch/irq.h"
#include "sched/exec.h"
#include "sched/process.h"
#include "sched/signal.h"
#include "sched/syscall.h"
#include "sys/clock.h"
#include "sys/cpu.h"
#include "sys/panic.h"
#include "sys/tty.h"

// This kernel pseudo-process is scheduled if there are no real process left to run
// It just halts the machine and waits for the next time slice
static void _spin(void) {
    for (;;)
        halt();
}

static sched_process* idle = NULL;

static tree* proc_tree = NULL;
static linked_list* sleep_queue = NULL;


// A simple sort of round robbin sxheduler
// pro: super simple, FIXME: con: sucks
static sched_thread* _get_next_process(bool evict) {
    list_node* lnode = list_pop_front(cpu->scheduler.run_queue);

    if (!lnode) {
        if (cpu->scheduler.current->proc != idle && !evict)
            return cpu->scheduler.current;
        else
            return proc_get_thread(idle, 0);
    }

    return lnode->data;
}

static void _switch_current(sched_thread* next) {
    if (next == cpu->scheduler.current)
        return;

    if (cpu->scheduler.current->proc != idle)
        list_push(cpu->scheduler.run_queue, &cpu->scheduler.current->lnode);

    cpu->scheduler.current = next;
}


void sched_enqueue(sched_thread* thread) {
    // Find the core that will get this process
    // TODO: we should be using some kind of score system here

    cpu_core* core = NULL;

    for (usize i = 0; i < MAX_CORES; i++) {
        cpu_core* cur = &cores_local[i];

        if (!cur->valid) //|| !cur->scheduler.running)
            continue;

        if (!core) {
            core = cur;
            continue;
        }

        if (cur->scheduler.run_queue->length < core->scheduler.run_queue->length)
            core = cur;
    }

    assert(core);

    thread->cpu_id = core->id;
    thread->state = T_RUNNING;

    list_append(core->scheduler.run_queue, &thread->lnode);
}

bool sched_dequeue(sched_thread* thread, bool and_current) {
    isize cpu_id = thread->cpu_id;

    // The thread is not in any run queue
    if (cpu_id < 0)
        return false;

    cpu_core* core = &cores_local[thread->cpu_id];

    assert(core);

    // If the thread is currently scheduled we have to reschedule
    if (core->scheduler.current == thread) {
        if (!and_current)
            return false;

        // evict the current process
        _switch_current(proc_get_thread(idle, 0));

        // remove from the run queue
        list_remove(core->scheduler.run_queue, &thread->lnode);

        schedule();
    } else {
        list_remove(core->scheduler.run_queue, &thread->lnode);
    }

    thread->cpu_id = -1;
    thread->state = T_STOPPED;

    return true;
}

void sched_enqueue_proc(sched_process* proc) {
    foreach (node, &proc->threads) {
        sched_thread* thread = node->data;
        sched_enqueue(thread);
    }
}

bool sched_dequeue_proc(sched_process* proc) {
    bool removed_one = false;

    foreach (node, &proc->threads) {
        sched_thread* thread = node->data;
        removed_one = sched_dequeue(thread, true);
    }

    return removed_one;
}


static bool _proc_comp(const void* data, const void* private) {
    sched_process* proc = (sched_process*)data;
    pid_t pid = (pid_t) private;

    return (proc->pid == pid);
}

sched_process* sched_get_proc(pid_t pid) {
    assert(proc_tree);

    tree_node* res = tree_find_comp(proc_tree, _proc_comp, (void*)pid);

    if (!res)
        return NULL;

    return res->data;
}


// FIXME: this is bad
static void _wake_sleepers(void) {
    time_t now = clock_now_ms();

    list_node* node = sleep_queue->head;
    while (node) {
        sched_thread* thread = node->data;
        list_node* next = node->next;

        if (thread->sleep_target <= now) {
            list_remove(sleep_queue, node);
            sched_enqueue(thread);
        }

        node = next;
    }
}

void schedule(void) {
    _wake_sleepers();

    _switch_current(_get_next_process(false));

    sched_thread* thread = cpu->scheduler.current;

    if (thread->proc->type == PROC_USER) {
        // Is this thread able to handle a signal
        usize signum = thread_signal_get_pending(thread);

        if (signum)
            thread_signal_switch(thread, signum);
    }

    cpu->scheduler.ticks_left = SCHED_SLICE;
    cpu->scheduler.needs_resched = false;
}

NORETURN
void sched_switch() {
    sched_thread* thread = cpu->scheduler.current;
    sched_process* proc = thread->proc;

    u64 ksp = thread->kstack.ptr;

    // Kernel processes don't have to switch the page table
    // Userspace processes must set the TSS to be able to switch back to ring 0
    if (proc->type == PROC_USER) {
        write_cr3((u64)proc->memory.table);
        set_tss_stack(ksp + sizeof(int_state));
    }

    context_switch(ksp);
    __builtin_unreachable();
}

// Since kernel processes use a single stack the stack pointer has
// to be updated once we context switch to a different process
// This is done automatically for user processes due to the TSS
void sched_save(int_state* s) {
    assert(s);

    sched_thread* thread = cpu_current_thread();

    if (thread->proc->type == PROC_KERNEL)
        thread->kstack.ptr = (u64)s;
}


void sched_thread_sleep(sched_thread* thread, u64 milis) {
    thread->state = T_SLEEPING;

    // Remove the thread from the run queue
    sched_dequeue(thread, true);

    thread->sleep_target = clock_now_ms() + milis;

    list_append(sleep_queue, &thread->lnode);

    cpu->scheduler.needs_resched = true;
}


// Spawn the init process (PID 1)
static void _spawn_init(void) {
    sched_process* init = spawn_uproc("init");
    sched_thread* thread = init->threads.head->data;

    vfs_node* file = vfs_lookup("/sbin/init.elf");

    if (!file)
        panic("init.elf not found!");

    bool exec = exec_elf(thread, file, NULL, NULL);

    if (!exec)
        panic("Failed to start init");

    virtual_tty* tty0 = get_tty(0);

    if (tty0) {
        proc_open_fd_node(init, tty0->pty->slave, STDIN_FD, FD_READ);
        proc_open_fd_node(init, tty0->pty->slave, STDOUT_FD, FD_WRITE);
        proc_open_fd_node(init, tty0->pty->slave, STDERR_FD, FD_WRITE);
    }

    proc_tree = tree_create_rooted(init->tnode);

    sched_enqueue(thread);
}

void scheduler_init() {
    idle = spawn_kproc("[idle]", _spin);
    sched_thread* idle_thread = idle->threads.head->data;

    sleep_queue = list_create();

    for (usize i = 0; i < MAX_CORES; i++) {
        cpu_core* core = &cores_local[i];

        if (!core->valid)
            continue;

        core->scheduler.run_queue = list_create();
        core->scheduler.current = idle_thread;

        // core->scheduler.running = true;
    }

    _spawn_init();

    syscall_init();
}


NORETURN
void scheduler_start() {
    for (usize i = 0; i < MAX_CORES; i++) {
        cpu_core* core = &cores_local[i];

        if (!core->valid)
            continue;

        core->scheduler.running = true;
    }

    log_info("Starting the scheduler");

    tty_set_current(0);

    // Bootstrap the scheduler
    schedule();

    timer_enable();

    sched_switch();
    __builtin_unreachable();
}


static void _recursive_dump(tree_node* parent, usize depth) {
    if (!parent->children)

        return;

    foreach (node, parent->children) {
        tree_node* child = node->data;
        sched_process* proc = child->data;

        log_debug("%-*s|- %s (%zu)", (int)depth, "", proc->name, proc->pid);

        _recursive_dump(child, depth + 1);
    }
}

void dump_process_tree() {
    log_debug("Recursive dump of the process tree:");

    tree_node* root_tnode = proc_tree->root;
    sched_process* root_proc = root_tnode->data;

    log_debug("%s (%zu)", root_proc->name, root_proc->pid);
    _recursive_dump(proc_tree->root, 0);
}
