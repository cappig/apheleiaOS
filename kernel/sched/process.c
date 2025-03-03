#include "process.h"

#include <aos/syscalls.h>
#include <base/addr.h>
#include <base/macros.h>
#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>
#include <data/vector.h>
#include <errno.h>
#include <fs/ustar.h>
#include <log/log.h>
#include <parse/elf.h>
#include <string.h>
#include <x86/asm.h>
#include <x86/paging.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "mem/heap.h"
#include "mem/physical.h"
#include "mem/virtual.h"
#include "sched/scheduler.h"
#include "sched/signal.h"
#include "sys/panic.h"
#include "vfs/fs.h"


static usize _next_pid(void) {
    static usize pid = 0;
    return pid++;
}


process* process_create(const char* name, u8 type, usize pid) {
    process* proc = kcalloc(sizeof(process));

    proc->name = strdup(name);
    proc->id = pid;

    proc->state = PROC_READY;
    proc->type = type;

    return proc;
}


// Free all the buffers that we can and leave the process in a zombified state
bool process_free(process* proc) {
    if (!proc)
        return false;

    void* stack_paddr = (void*)ID_MAPPED_PADDR(proc->stack);
    free_frames(stack_paddr, proc->stack_size);

    // Kernel processes only need to free the kernel stack
    if (proc->type != PROC_USER)
        return true;

    if (proc->user.stack_size)
        free_frames((void*)proc->user.stack_paddr, proc->user.stack_size);

    free_table(proc->user.mem_map);
    free_frames(proc->user.mem_map, 1);

    proc->state = PROC_DONE;

    // Init adopts any orphaned children
    tree_node* proc_node = proc->user.tree_entry;
    tree_node* init_node = proc_tree->root;

    foreach (node, proc_node->children) {
        tree_node* child_node = node->data;
        tree_insert_child(init_node, child_node);

#ifdef SCHED_DEBUG
        log_debug("[SCHED_DEBUG] init adopted orphan: name=%s pid=%lu", proc->name, proc->id);
#endif
    }

    list_destroy(proc_node->children, false);
    proc_node->children = NULL;

    if (proc->user.args_paddr)
        free_frames((void*)proc->user.args_paddr, proc->user.args_pages);

    return true;
}

// obvously process_free() has to be called before so that nothing is left dangling
pid_t process_reap(process* proc) {
    assert(proc->type == PROC_USER);

    pid_t ret = proc->id;

#ifdef SCHED_DEBUG
    log_debug("[SCHED_DEBUG] reaping process: name=%s pid=%lu", proc->name, proc->id);
#endif

    tree_node* tnode = proc->user.tree_entry;
    tree_remove_child(tnode->parent, tnode);

    assert(!tnode->children);
    tree_destroy_node(tnode);

    kfree(proc);
    kfree(proc->name);

    return ret;
}


void process_init_stack(process* parent, process* child) {
    void* paddr = alloc_frames(SCHED_KSTACK_PAGES);

    child->stack = (u64)ID_MAPPED_VADDR(paddr);
    child->stack_size = SCHED_KSTACK_SIZE;

    usize offset = SCHED_KSTACK_SIZE;

    if (parent) {
        void* dest = (void*)child->stack;
        void* src = (void*)parent->stack;

        memcpy(dest, src, SCHED_KSTACK_SIZE);

        offset = parent->stack_ptr - parent->stack;

        // Clone the user stack
        if (child->type == PROC_USER) {
            child->user.stack_vaddr = parent->user.stack_vaddr;
            child->user.stack_size = parent->user.stack_size;

            usize pages = DIV_ROUND_UP(parent->user.stack_size, PAGE_4KIB);

            child->user.stack_paddr = (u64)alloc_frames(pages);

            void* child_vaddr = (void*)ID_MAPPED_VADDR(child->user.stack_paddr);
            void* parent_vaddr = (void*)ID_MAPPED_VADDR(parent->user.stack_paddr);

            memcpy(child_vaddr, parent_vaddr, parent->user.stack_size);
        }
    }

    child->stack_ptr = child->stack + offset;
}

void process_init_page_map(process* parent, process* child) {
    assert(child->type == PROC_USER);

    page_table* table;

    if (parent)
        table = parent->user.mem_map;
    else
        table = (page_table*)read_cr3();

    child->user.mem_map = clone_table(table);
}

void process_init_file_descriptors(process* parent, process* child) {
    assert(child->type == PROC_USER);

    if (parent)
        child->user.fd_table = vec_clone(parent->user.fd_table);
    else
        child->user.fd_table = vec_create(sizeof(file_desc));
}

void process_init_signal_handlers(process* parent, process* child) {
    assert(child->type == PROC_USER);

    if (parent) {
        usize len = SIGNAL_COUNT * sizeof(sighandler_fn);
        memcpy(child->user.signals.handlers, parent->user.signals.handlers, len);

        child->user.signals.masked = parent->user.signals.masked;

        child->user.vdso = parent->user.vdso;
        child->user.signals.trampoline = parent->user.signals.trampoline;
    } else {
        process_signal_defaults(child);
    }
}


isize process_open_fd_node(process* proc, vfs_node* node, isize fd, u16 flags) {
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
        fd = proc->user.fd_table->size;

    if (!vec_insert(proc->user.fd_table, fd, &fdesc))
        return -ENOMEM;

    return fd;
}

isize process_open_fd(process* proc, const char* path, isize fd, u16 flags) {
    vfs_node* node = vfs_lookup(path);
    return process_open_fd_node(proc, node, fd, flags);
}

file_desc* process_get_fd(process* proc, usize fd) {
    assert(proc->type == PROC_USER);

    return vec_at(proc->user.fd_table, fd);
}


void process_set_state(process* proc, int_state* state) {
    proc->stack_ptr = proc->stack + proc->stack_size - sizeof(int_state);

    void* dest = (void*)proc->stack_ptr;
    memcpy(dest, state, sizeof(int_state));
}

void process_push_state(process* proc, int_state* state) {
    proc->stack_ptr -= sizeof(int_state);

    void* dest = (void*)proc->stack_ptr;
    memcpy(dest, state, sizeof(int_state));
}


// Spawns an orphaned kernel process that runs in ring 0
// Since these process have kernel level privileges they can run arbitrary kernel code so
// we only have to pass in a pointer to function that lives in the kernel's memory space
process* spawn_kproc(const char* name, void* entry) {
    usize pid = _next_pid();
    process* proc = process_create(name, PROC_KERNEL, pid);

    process_init_stack(NULL, proc);

    // All process must have a kernel stack that we use to save the process state on kernel entry
    // We also use it while in the kernel. A process may also have a 'user stack' that it uses
    // during execution in userspace. Kernel processes can just use a single stack for both purposes
    u64 ksp = (u64)proc->stack_ptr - sizeof(int_state);

    int_state state = {
        .s_regs.rip = (u64)entry,
        .s_regs.cs = GDT_kernel_code,
        .s_regs.rflags = 0x200,
        .s_regs.rsp = ksp,
        .s_regs.ss = GDT_kernel_data
    };

    process_set_state(proc, &state);

    return proc;
}

// Spawns an orphaned user process that runs in ring 3
// We use this to spawn 'init'
process* spawn_uproc(const char* name) {
    usize pid = _next_pid();
    process* proc = process_create(name, PROC_USER, pid);

    process_init_stack(NULL, proc);
    process_init_page_map(NULL, proc);
    process_init_file_descriptors(NULL, proc);
    process_init_signal_handlers(NULL, proc);

    return proc;
}


// TODO: don't just copy everything. Leverage the MMU/page faults
process* process_fork(process* parent) {
    assert(parent->type == PROC_USER); // why?

    usize pid = _next_pid();
    process* child = process_create(parent->name, PROC_USER, pid);

    child->user.tree_entry = tree_create_node(child);
    tree_insert_child(parent->user.tree_entry, child->user.tree_entry);

    process_init_stack(parent, child);
    process_init_page_map(parent, child);
    process_init_file_descriptors(parent, child);
    process_init_signal_handlers(parent, child);

    return child;
}
