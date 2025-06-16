#pragma once

#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>
#include <data/vector.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <x86/paging.h>

#include "arch/idt.h"
#include "sys/disk.h"

#define SCHED_KSTACK_PAGES 2
#define SCHED_USTACK_PAGES 8

#define PROC_USTACK_BASE 0x700000000000ULL
#define PROC_VDSO_BASE   0x7ff000000000ULL

#define SUPERUSER_UID 0
#define SUPERUSER_GID 0

#define PROC_SIGNAL_DEFAULT ((sighandler_t)0)

typedef enum {
    PROC_USER,
    PROC_KERNEL,
} process_type;

typedef struct {
    u64 base;
    u64 size; // size in bytes, the actual region always takes up a page aligned amount of space

    usize offset;
    vfs_node* file;

    u32 flags;
    u32 prot;
} memory_region;

typedef struct {
    page_table* table;

    // Mark occupied regions of memory, this aids in the implementation of mmap()
    vector* regions;

    // The address of the virtual dynamic shared object
    u64 vdso;

    // The address of the signal trampoline - usually the vdso
    u64 trampoline;

    // The address of the page(es) that hold the strings for the command
    // line arguments and the environment variables
    u64 args_paddr;
    usize args_pages;
} process_memory;


typedef struct {
    u32 pending; // the Nth bit being set marks the (N-1)th signal as pending

    // What action shall be taken when handling the given siganl
    // 0 if default, 1 if ignored and a valid address if a defined handler should be called
    sighandler_t handlers[NSIG];
} process_signals;

enum fd_flags {
    FD_READ = 1 << 0,
    FD_WRITE = 1 << 1,
    FD_APPEND = 1 << 2,
    FD_NONBLOCK = 1 << 3,
};

typedef struct {
    vfs_node* node;
    usize offset;
    u16 flags;
} file_desc;

typedef struct {
    uid_t uid;
    uid_t euid;
    uid_t suid;

    uid_t gid;
    uid_t egid;
    uid_t sgid;
} process_identity;

typedef enum {
    PROC_RUNNING, // at least one thread is alive (may be stopped or sleeping)
    PROC_ZOMBIE, // all threads are dead (zombies or just nonexistent)
} process_state;

typedef struct {
    char* name;

    pid_t pid;
    pid_t group;

    process_type type;
    process_identity identity;

    process_state state;
    usize exit_code;

    tree_node* tnode;

    process_signals signals;

    process_memory memory;

    vector* file_descriptors;

    linked_list threads;
} sched_process;


typedef struct {
    usize size;
    u64 paddr;
    union {
        u64 ptr; // kstack
        u64 vaddr; // ustack
    };
} thread_stack;

typedef struct {
    // 0 if not waiting
    // -1 if waiting for any child
    // > 0 if waiting for a specific child (PID)
    pid_t proc;
    int* code_ptr; // the exit code of the while shall be written to this address
} thread_wait;

typedef enum {
    T_RUNNING,
    T_SLEEPING, // a blocked state, the thread is waitong for an event
    T_STOPPED, // the thread exists, it won't get scheduled in this state
    T_ZOMBIE,
} thread_state;

typedef struct {
    tid_t tid;
    sched_process* proc;

    list_node lnode;

    thread_state state;
    void* exit_val;

    u32 current_signal;
    u32 signal_mask;

    thread_stack kstack;
    thread_stack ustack;

    thread_wait waiting;
    time_t sleep_target;

    isize cpu_id; // -1 if none
} sched_thread;


inline u64 prot_to_page_flags(u64 prot) {
    u64 ret = PT_USER;

    if (!(prot & PROT_EXEC))
        ret |= PT_NO_EXECUTE;

    if (prot & PROT_WRITE)
        ret |= PT_WRITE;

    return ret;
}

inline u64 page_flags_to_prot(u64 flags) {
    u64 ret = PROT_READ;

    if (flags & PT_WRITE)
        ret |= PROT_WRITE;

    if (flags & PT_NO_EXECUTE)
        ret |= PROT_EXEC;

    return ret;
}


sched_process* proc_create_with_pid(const char* name, process_type type, pid_t pid);
sched_process* proc_create(const char* name, process_type type);

sched_process* spawn_kproc(const char* name, void* entry);
sched_process* spawn_uproc(const char* name);

void proc_free(sched_process* proc);
void proc_destroy(sched_process* proc);

void thread_push_state(sched_thread* thread, int_state* state);
void thread_set_state(sched_thread* thread, int_state* state);

void thread_init_stack(sched_thread* parent, sched_thread* child);

bool thread_wait_wake(sched_thread* thread, sched_process* target);
bool proc_wait_wake(sched_process* parent, sched_process* target);

void proc_init_memory(sched_process* parent, sched_process* child);
void proc_init_file_descriptors(sched_process* parent, sched_process* child);
void proc_init_signal_handlers(sched_process* parent, sched_process* child);

isize proc_open_fd_node(sched_process* proc, vfs_node* node, isize fd, u16 flags);
isize proc_open_fd(sched_process* proc, const char* path, isize fd, u16 flags);
file_desc* process_get_fd(sched_process* proc, usize fd);

sched_thread* proc_spawn_thread(sched_process* proc, sched_thread* caller);

list_node* proc_get_thread_node(sched_process* proc, tid_t tid);
sched_thread* proc_get_thread(sched_process* proc, tid_t tid);

bool proc_exit_thread(sched_thread* thread, void* exit_val);
void* proc_reap_thread(sched_thread* thread);

sched_process* proc_fork(sched_process* parent, tid_t tid);

bool proc_terminate(sched_process* proc, usize exit_code);

u64 proc_mmap(sched_process* proc, u64 addr, u64 size, u32 prot, u32 flags, vfs_node* file, usize off);
bool proc_handle_page_fault(sched_process* proc, int_state* state);

bool proc_insert_mem_region(sched_process* proc, memory_region* region);

bool proc_validate_ptr(sched_process* proc, const void* ptr, usize len, bool write);

bool proc_is_descendant(sched_process* proc, sched_process* target);

void proc_dump_regions(sched_process* proc);
