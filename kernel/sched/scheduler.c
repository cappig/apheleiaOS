#include "scheduler.h"

#include <aos/signals.h>
#include <base/addr.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>
#include <log/log.h>
#include <parse/elf.h>
#include <string.h>
#include <x86/asm.h>
#include <x86/paging.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "arch/irq.h"
#include "drivers/initrd.h"
#include "mem/heap.h"
#include "mem/virtual.h"
#include "sched/exec.h"
#include "sched/process.h"
#include "sched/signal.h"
#include "sched/syscall.h"
#include "sys/clock.h"
#include "sys/cpu.h"
#include "sys/panic.h"
#include "sys/tty.h"
#include "vfs/fs.h"


// Defined in switch.asm
extern NORETURN void context_switch(u64 kernel_stack);

tree* proc_tree = NULL;


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
    assert(proc_tree);

    tree_node* res = tree_find_comp(proc_tree, _proc_comp, (void*)pid);

    if (!res)
        return NULL;

    return res->data;
}


static usize _get_table(process* proc, void* ptr, bool write, page_table** page_ptr) {
    usize size = get_page(proc->user.mem_map, (u64)ptr, page_ptr);

    page_table* page = *page_ptr;

    if (!size || !page)
        return 0;

    if (!page->bits.present)
        return 0;

    if (!page->bits.user)
        return 0;

    if (write && !page->bits.writable)
        return 0;

    return size;
}

// Is a given pointer in the address space of the process valid? Walk the page map and find out :P
bool process_validate_ptr(process* proc, const void* ptr, usize len, bool write) {
    if (!ptr)
        return false;

    // The entire higher half of the memory space is reserved for the kernel
    if ((u64)ptr > LOWER_HALF_TOP)
        return false;

    void* cur = (void*)ptr;

    while (cur <= ptr + len) {
        page_table* page;
        usize size = _get_table(proc, cur, write, &page);

        if (!size || !page)
            return false;

        if (!page->bits.present)
            return false;

        if (!page->bits.user)
            return false;

        if (write && !page->bits.writable)
            return false;

        cur += size;
    }

    return true;
}


// Check if a sleeping process has to be woken up
static bool _wake_sleeper(void) {
    list_node* sleeper_to_wake = NULL;
    isize longest_wait = 1;

    foreach (node, cpu->sched->sleep_queue) {
        sleeping_process* curr = node->data;

        // Find the process that has been waiting the longest
        // Prefer processes higher up in the queue
        if (curr->time_left <= 0 && curr->time_left < longest_wait) {
            longest_wait = curr->time_left;

            sleeper_to_wake = node;
            cpu->sched->current = curr->proc;
        }
    }

    if (!sleeper_to_wake)
        return false;

    sleeping_process* sproc = sleeper_to_wake->data;
    process* proc = sproc->proc;

    kfree(sproc);

    list_remove(cpu->sched->sleep_queue, sleeper_to_wake);
    list_destroy_node(sleeper_to_wake);

    proc->state = PROC_RUNNING;

    cpu->sched->current = proc;

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
    cpu->sched->proc_ticks_left--;

    foreach (node, cpu->sched->sleep_queue) {
        sleeping_process* proc = node->data;
        proc->time_left--;
    }
}

// Figure out which process should get run on the next context switch
void schedule() {
    bool sleeper = _wake_sleeper();

    // Is the current timeslice is done
    if (cpu->sched->proc_ticks_left <= 0 && !sleeper) {
        _get_next_process(cpu->sched);
        cpu->sched->proc_ticks_left = SCHED_SLICE;
    }

    // Are there any pending signals
    if (cpu->sched->current->type == PROC_USER) {
        usize signum = signal_get_pending(cpu->sched->current);
        prepare_signal(cpu->sched->current, signum);
    }

#ifdef SCHED_DEBUG
    log_debug(
        "[SCHED_DEBUG] scheduling process: name=%s pid=%lu",
        cpu->sched->current->name,
        cpu->sched->current->id
    );
#endif
}

NORETURN
void scheduler_switch() {
    assert(cpu->sched->current);

    // The process will have valid state to save after this time slice
    if (cpu->sched->current->state == PROC_READY)
        cpu->sched->current->state = PROC_RUNNING;

    u64 ksp = (u64)cpu->sched->current->stack_ptr;

    // Kernel processes don't have to switch the page table
    // Userspace processes must set the TSS to be able to switch back to ring 0
    if (cpu->sched->current->type == PROC_USER) {
        write_cr3((u64)cpu->sched->current->user.mem_map);
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

    if (cpu->sched->current->type != PROC_KERNEL)
        return;

    cpu->sched->current->stack_ptr = (u64)s;
}


void scheduler_queue(process* proc) {
    list_node* node = list_create_node(proc);
    list_append(cpu->sched->run_queue, node);
}

// NOTE: the sleeping process _must_ be in the run queue as well
void scheduler_sleep(process* proc, usize milis) {
    proc->state = PROC_BLOCKED;

    sleeping_process* sproc = kcalloc(sizeof(sleeping_process));

    sproc->proc = proc;
    sproc->time_left = DIV_ROUND_UP(milis, MS_PER_TICK);

    list_node* node = list_create_node(sproc);
    list_append(cpu->sched->sleep_queue, node);
}


void scheduler_kill(process* proc, usize status) {
    list_node* node = list_find(cpu->sched->run_queue, proc);

    assert(node);

    list_remove(cpu->sched->run_queue, node);
    list_destroy_node(node);

    process_free(proc);

    // kernel processes don't have children so they can just die right now (oof)
    if (proc->type != PROC_USER) {
        cpu->sched->proc_ticks_left = 0;
        schedule();

        return;
    }

    proc->status = status;
    proc->state = PROC_DONE;

    // Pass the unfortunate news to the parent
    tree_node* parent_node = proc->user.tree_entry->parent;

#ifdef SCHED_DEBUG
    log_debug("[SCHED_DEBUG] killing process: name=%s pid=%lu", proc->name, proc->id);
#endif

    if (!parent_node)
        panic("Attempted to kill init (pid = %zu)!", proc->id);

    process* parent = parent_node->data;

    assert(parent);

    signal_send(parent, SIGCHLD);

    // Is the parent waiting for a child
    if (parent->user.waiting) {
        parent->state = PROC_RUNNING;
        parent->user.waiting = 0;

        // This will be the value returned by the wait() syscall originally called by the parent
        // Kind of hacky but eeehh
        int_state* parent_state = (int_state*)parent->stack_ptr;
        parent_state->g_regs.rax = cpu->sched->current->id;

        // So unlike the syscall handler we aren't guaranteed to have the process
        // page table mapped at this moment. This means that we have to walk it
        // manually and figure out the physical address. >:|
        u64 status_ptr = (u64)parent->user.child_status;
        usize page_offset = (u64)status_ptr & 0xfff;

        page_table* page;
        if (_get_table(parent, (void*)status_ptr, true, &page)) {
            void* page_vaddr = page_get_vaddr(page);

            int* vaddr = page_vaddr + page_offset;
            *vaddr = proc->status;

            process_reap(proc);
        }
    }

    cpu->sched->proc_ticks_left = 0;
    schedule();
}


static void _recursive_dump(tree_node* parent, usize depth) {
    if (!parent->children)
        return;

    foreach (node, parent->children) {
        tree_node* child = node->data;
        process* proc = child->data;

        log_debug("%-*s|- %s (%zu)", (int)depth, "", proc->name, proc->id);

        _recursive_dump(child, depth + 1);
    }
}

void dump_process_tree() {
    log_debug("Recursive dump of the process tree:");

    tree_node* root_tnode = proc_tree->root;
    process* root_proc = root_tnode->data;

    log_debug("%s (%zu)", root_proc->name, root_proc->id);
    _recursive_dump(proc_tree->root, 0);
}


// Spawn the init process (PID 1)
static void _spawn_init(void) {
    process* init = spawn_uproc("init");

    vfs_node* file = vfs_lookup_relative(INITRD_MOUNT, "usr/init.elf");

    if (!file)
        panic("init.elf not found!");

    bool exec = exec_elf(init, file, NULL, NULL);

    if (!exec)
        panic("Failed to start init");

    proc_tree = tree_create(init);
    init->user.tree_entry = proc_tree->root;

    scheduler_queue(init);
}

void scheduler_init() {
    cpu->sched = kcalloc(sizeof(scheduler));

    cpu->sched->run_queue = list_create();
    cpu->sched->sleep_queue = list_create();

    cpu->sched->idle = spawn_kproc("[idle]", _spin);

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

    cpu->sched_running = true;

    scheduler_switch();
    __builtin_unreachable();
}
