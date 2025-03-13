#include "sched/process.h"

#include <aos/signals.h>
#include <aos/syscalls.h>
#include <base/addr.h>
#include <base/macros.h>
#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>
#include <data/vector.h>
#include <errno.h>
#include <string.h>
#include <x86/asm.h>
#include <x86/paging.h>

#include "arch/gdt.h"
#include "mem/heap.h"
#include "mem/physical.h"
#include "mem/virtual.h"
#include "sched/scheduler.h"
#include "sched/signal.h"
#include "sys/panic.h"


static inline pid_t _next_pid(void) {
    static pid_t pid = -1; // the idle process gets a pid = -1 since its not a real process
    return pid++;
}


sched_process* proc_create_with_pid(const char* name, process_type type, pid_t pid) {
    sched_process* proc = kcalloc(sizeof(sched_process));

    proc->name = strdup(name);
    proc->pid = pid;

    proc->type = type;

    // Spawn the initial thread
    proc_spawn_thread(proc, NULL);

    return proc;
}

sched_process* proc_create(const char* name, process_type type) {
    return proc_create_with_pid(name, type, _next_pid());
}

// Free as much stuff as we can, we use this when processes get zombified
// NOTE: this _does not_ kill threads
void proc_free(sched_process* proc) {
    vec_destroy(proc->file_descriptors);

    free_table(proc->memory.table);
}

void proc_destroy(sched_process* proc) {
    if (proc->name)
        kfree(proc->name);

    kfree(proc->tnode->children);
    kfree(proc->tnode);

    if (proc->tnode->parent)
        tree_remove_child(proc->tnode->parent, proc->tnode);
}


// Spawns an orphaned kernel process that runs in ring 0
// Since these process have kernel level privileges they can run arbitrary kernel code so
// we only have to pass in a pointer to function that lives in the kernel's memory space
sched_process* spawn_kproc(const char* name, void* entry) {
    sched_process* proc = proc_create(name, PROC_KERNEL);
    sched_thread* thread = proc->threads.head->data;

    thread_init_stack(NULL, thread);

    // All process must have a kernel stack that we use to save the process state on kernel entry
    // We also use it while in the kernel. A process may also have a 'user stack' that it uses
    // during execution in userspace. Kernel processes can just use a single stack for both purposes
    u64 ksp = (u64)thread->kstack.ptr - sizeof(int_state);

    int_state state = {
        .s_regs.rip = (u64)entry,
        .s_regs.cs = GDT_kernel_code,
        .s_regs.rflags = 0x200,
        .s_regs.rsp = ksp,
        .s_regs.ss = GDT_kernel_data,
    };

    thread_set_state(thread, &state);

    return proc;
}

// Spawns an orphaned user process that runs in ring 3
// We use this to spawn PID 0 (init)
sched_process* spawn_uproc(const char* name) {
    sched_process* proc = proc_create(name, PROC_USER);
    sched_thread* thread = proc->threads.head->data;

    proc_init_memory(NULL, proc);

    thread_init_stack(NULL, thread);

    proc_init_file_descriptors(NULL, proc);
    proc_init_signal_handlers(NULL, proc);

    proc->tnode = tree_create_node(proc);

    return proc;
}


// 'rewind' the stack to the top and push on the new state
void thread_set_state(sched_thread* thread, int_state* state) {
    thread->kstack.ptr = ID_MAPPED_VADDR(thread->kstack.paddr) + thread->kstack.size;

    thread_push_state(thread, state);
}

void thread_push_state(sched_thread* thread, int_state* state) {
    thread->kstack.ptr -= sizeof(int_state);

    void* dest = (void*)thread->kstack.ptr;
    memcpy(dest, state, sizeof(int_state));
}


void thread_init_stack(sched_thread* parent, sched_thread* child) {
    assert(child);

    child->kstack.paddr = (u64)alloc_frames(SCHED_KSTACK_PAGES);
    child->kstack.size = SCHED_KSTACK_PAGES * PAGE_4KIB;

    usize offset = child->kstack.size;

    if (parent) {
        // Clone the kernel stack
        void* dest = (void*)ID_MAPPED_VADDR(child->kstack.paddr);
        void* src = (void*)ID_MAPPED_VADDR(parent->kstack.paddr);

        memcpy(dest, src, child->kstack.size);

        offset = parent->kstack.ptr - ID_MAPPED_VADDR(parent->kstack.paddr);

        // Clone the user stack
        if (parent->proc->type == PROC_USER) {
            child->ustack.vaddr = parent->ustack.vaddr;
            child->ustack.size = parent->ustack.size;

            usize pages = DIV_ROUND_UP(parent->ustack.size, PAGE_4KIB);

            child->ustack.paddr = (u64)alloc_frames(pages);

            void* child_vaddr = (void*)ID_MAPPED_VADDR(child->ustack.paddr);
            void* parent_vaddr = (void*)ID_MAPPED_VADDR(parent->ustack.paddr);

            // The pages that back the new user stack are different, remap
            map_region(
                child->proc->memory.table,
                pages,
                child->ustack.vaddr,
                child->ustack.paddr,
                PT_PRESENT | PT_NO_EXECUTE | PT_WRITE | PT_USER
            );

            memcpy(child_vaddr, parent_vaddr, pages);
        }
    }

    child->kstack.ptr = ID_MAPPED_VADDR(child->kstack.paddr) + offset;
}


static usize _get_table(sched_process* proc, void* ptr, bool write, page_table** page_ptr) {
    usize size = get_page(proc->memory.table, (u64)ptr, page_ptr);

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

// Is a given pointer in the address space of the thread valid? Walk the page map and find out :P
bool proc_validate_ptr(sched_process* proc, const void* ptr, usize len, bool write) {
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


bool thread_wait_wake(sched_thread* thread, sched_process* target) {
    if (thread->waiting.proc != target->pid)
        return false;

    // This will be the value returned by the wait() syscall originally called by the parent
    // Kind of hacky but eeehh
    int_state* state = (int_state*)thread->kstack.ptr;
    state->g_regs.rax = target->pid;

    // So unlike the syscall handler we aren't guaranteed to have the process
    // page table mapped at this moment. This means that we have to walk it
    // manually and figure out the physical address. >:|
    u64 status_ptr = (u64)thread->waiting.code_ptr;
    usize page_offset = (u64)status_ptr & 0xfff;

    page_table* page;
    if (!_get_table(thread->proc, (void*)status_ptr, true, &page))
        return false;

    void* page_vaddr = page_get_vaddr(page);

    int* vaddr = page_vaddr + page_offset;
    *vaddr = target->exit_code;

    return true;
}

// Wake up any threads that are waiting for a (child) process
bool proc_wait_wake(sched_process* parent, sched_process* target) {
    bool woken = false;

    foreach (node, &parent->threads) {
        sched_thread* thread = node->data;
        woken = thread_wait_wake(thread, target);
    }

    return woken;
}


void proc_init_memory(sched_process* parent, sched_process* child) {
    assert(child->type == PROC_USER);

    // Clone the page table
    page_table* table;

    if (parent)
        table = parent->memory.table;
    else
        table = (page_table*)read_cr3();

    child->memory.table = clone_table(table);

    // Clone the memory region table
    /* if (parent) */
    /*     child->memory.regions = vec_clone(parent->memory.regions); */
    /* else */
    /*     child->memory.regions = vec_create(sizeof(memory_region)); */

    // Clone the defined addresses
    if (parent) {
        child->memory.vdso = parent->memory.vdso;

        child->memory.trampoline = parent->memory.trampoline;

        child->memory.args_paddr = parent->memory.args_paddr;
        child->memory.args_pages = parent->memory.args_pages;
    }
}

void proc_init_file_descriptors(sched_process* parent, sched_process* child) {
    assert(child->type == PROC_USER);

    if (parent)
        child->file_descriptors = vec_clone(parent->file_descriptors);
    else
        child->file_descriptors = vec_create(sizeof(file_desc));
}

void proc_init_signal_handlers(sched_process* parent, sched_process* child) {
    assert(child->type == PROC_USER);

    usize len = SIGNAL_COUNT * sizeof(sighandler_fn);

    if (parent)
        memcpy(child->signals.handlers, parent->signals.handlers, len);
    else
        memset(child->signals.handlers, (uptr)PROC_SIGNAL_DEFAULT, len);
}


isize proc_open_fd_node(sched_process* proc, vfs_node* node, isize fd, u16 flags) {
    assert(proc->type == PROC_USER);

    // TODO: flags and mode
    file_desc fdesc = {
        .node = node,
        .offset = 0,
        .flags = flags,
    };

    if (!fdesc.node)
        return -ENOENT;

    // A negative fd tells this function to assign it
    if (fd < 0)
        fd = proc->file_descriptors->size;

    if (!vec_insert(proc->file_descriptors, fd, &fdesc))
        return -ENOMEM;

    return fd;
}

isize proc_open_fd(sched_process* proc, const char* path, isize fd, u16 flags) {
    vfs_node* node = vfs_lookup(path);
    return proc_open_fd_node(proc, node, fd, flags);
}

file_desc* process_get_fd(sched_process* proc, usize fd) {
    assert(proc->type == PROC_USER);

    return vec_at(proc->file_descriptors, fd);
}


sched_thread* proc_spawn_thread(sched_process* proc, sched_thread* caller) {
    tid_t new_tid = 0, max_tid = 0;

    // Make sure that the next TID hasn't been used, the new tid will be the max current tid + 1
    // Not _ideal_ since we can gets some empty space but meh 32 bits go a long way
    foreach (node, &proc->threads) {
        sched_thread* thread = node->data;

        if (thread->tid > max_tid) {
            max_tid = thread->tid;
            new_tid = thread->tid + 1;
        }
    }

    sched_thread* thread = kcalloc(sizeof(sched_thread));

    thread->tid = new_tid;
    thread->cpu_id = -1;
    thread->proc = proc;
    thread->state = T_RUNNING;

    // Threads inherit signal masks from the calling thread
    if (caller)
        thread->signal_mask = caller->signal_mask;

    // To prevent constant reallocation threads will allocate the list node only once
    // Once the thread gets pushed to a different run queue all we have to do is rewire the pointers
    thread->lnode.data = thread;

    // A _different_ list node... yeah
    list_push(&proc->threads, list_create_node(thread));

    return thread;
}


list_node* proc_get_thread_node(sched_process* proc, tid_t tid) {
    foreach (node, &proc->threads) {
        sched_thread* thread = node->data;

        if (thread->tid == tid)
            return node;
    }

    return NULL;
}

sched_thread* proc_get_thread(sched_process* proc, tid_t tid) {
    list_node* node = proc_get_thread_node(proc, tid);

    if (!node)
        return NULL;

    return node->data;
}


bool proc_exit_thread(sched_thread* thread, void* exit_val) {
    if (thread->state == T_ZOMBIE)
        return false;

    sched_dequeue(thread, true);

    thread->state = T_ZOMBIE;
    thread->exit_val = exit_val;

    return true;
}

void* proc_reap_thread(sched_thread* thread) {
    if (thread->state != T_ZOMBIE)
        return NULL;

    void* exit_val = thread->exit_val;

    kfree(thread);

    return exit_val;
}


// A new forked process will have only one thread, the new thread will
// be forked from the calling thread in the parent process
sched_process* proc_fork(sched_process* parent, tid_t tid) {
    sched_thread* caller = proc_get_thread(parent, tid);

    if (!caller)
        return NULL;

    sched_process* child = proc_create(parent->name, parent->type);
    sched_thread* child_thread = proc_get_thread(child, 0);

    child->tnode = tree_create_node(child);

    tree_insert_child(parent->tnode, child->tnode);

    proc_init_memory(parent, child);

    thread_init_stack(caller, child_thread);

    proc_init_file_descriptors(parent, child);
    proc_init_signal_handlers(parent, child);

    return child;
}


// Kill all active threads and zombify
bool proc_terminate(sched_process* proc, usize exit_code) {
    if (proc->state != PROC_RUNNING)
        return false;

    proc->exit_code = exit_code;
    proc->state = PROC_ZOMBIE;

    foreach (node, &proc->threads) {
        sched_thread* thread = node->data;

        proc_exit_thread(thread, NULL);
        proc_reap_thread(thread);
        list_destroy_node(node);
    }

    proc_free(proc);
    sched_dequeue_proc(proc);

    // Pass the unfortunate news to the parent
    tree_node* parent_node = proc->tnode->parent;

    if (UNLIKELY(!parent_node))
        panic("Attempted to kill init!");

    sched_process* parent = parent_node->data;

    assert(parent);

    signal_send(parent, -1, SIGCHLD);

    // All children get adopted by init
    sched_process* init = sched_get_proc(0);
    assert(init);

    tree_node* init_node = init->tnode;

    foreach (child, proc->tnode->children) {
        sched_process* child_proc = child->data;
        tree_insert_child(init_node, child_proc->tnode);
    }

    // Is the parent waiting for a child
    if (proc_wait_wake(parent, proc))
        proc_destroy(proc); // we can reap the zombie right now

    return true;
}
