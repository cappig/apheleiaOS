#pragma once

#include <arch/arch.h>
#include <arch/context.h>
#include <base/attributes.h>
#include <base/types.h>
#include <data/list.h>
#include <sys/types.h>
#include <sys/proc.h>

typedef void (*thread_entry_t)(void* arg);

typedef struct vfs_node vfs_node_t;

#define SCHED_FD_MAX     32
#define SCHED_REGION_COW (1ULL << 62)

typedef struct sched_fd {
    vfs_node_t* node;
    size_t offset;
    u32 flags;
} sched_fd_t;

typedef enum {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_SLEEPING,
    THREAD_STOPPED,
    THREAD_ZOMBIE,
} thread_state_t;

typedef struct sched_user_region {
    uintptr_t vaddr;
    uintptr_t paddr;
    size_t pages;
    u64 flags;
    struct sched_user_region* next;
} sched_user_region_t;

typedef struct sched_wait_queue {
    linked_list_t* list;
} sched_wait_queue_t;

typedef struct sched_thread {
    char name[PROC_NAME_MAX];
    thread_state_t state;

    list_node_t run_node;
    bool in_run_queue;

    list_node_t wait_node;
    bool in_wait_queue;

    list_node_t zombie_node;
    bool in_zombie_list;

    void* stack;
    size_t stack_size;
    uintptr_t context;

    thread_entry_t entry;
    void* arg;

    bool user_thread;
    pid_t pid;
    pid_t ppid;
    pid_t pgid;
    pid_t sid;
    uid_t uid;
    gid_t gid;
    int exit_code;

    arch_vm_space_t* vm_space;

    uintptr_t user_stack_base;
    size_t user_stack_size;

    struct sched_user_region* regions;

    sched_wait_queue_t wait_queue;

    list_node_t all_node;
    bool in_all_list;

    sched_fd_t fds[SCHED_FD_MAX];
    bool fd_used[SCHED_FD_MAX];

    char cwd[PATH_MAX];

    u64 wake_tick;

    u32 signal_pending;
    u32 signal_mask;
    u32 current_signal;
    int stop_signal;
    bool stop_reported;
    bool signal_saved_valid;
    uintptr_t signal_trampoline;
    arch_int_state_t signal_saved_state;
    sighandler_t signal_handlers[NSIG];

    int tty_index;
} sched_thread_t;

void scheduler_init(void);
void scheduler_start(void);

bool sched_is_running(void);

sched_thread_t* sched_current(void);
sched_thread_t* sched_create_kernel_thread(const char* name, thread_entry_t entry, void* arg);
sched_thread_t* sched_create_user_thread(const char* name);
pid_t sched_fork(arch_int_state_t* state);
pid_t sched_wait(pid_t pid, int* status);
pid_t sched_waitpid(pid_t pid, int* status, int options);
void sched_prepare_user_thread(sched_thread_t* thread, uintptr_t entry, uintptr_t user_stack_top);
void sched_discard_thread(sched_thread_t* thread);
void sched_make_runnable(sched_thread_t* thread);
void sched_stop_thread(sched_thread_t* thread, int signum);
void sched_continue_thread(sched_thread_t* thread);
void sched_set_thread_name(sched_thread_t* thread, const char* name);

bool sched_add_user_region(
    sched_thread_t* thread,
    uintptr_t vaddr,
    uintptr_t paddr,
    size_t pages,
    u64 flags
);
void sched_clear_user_regions(sched_thread_t* thread);

void sched_wait_queue_init(sched_wait_queue_t* queue);
void sched_wait_queue_destroy(sched_wait_queue_t* queue);

void sched_block(sched_wait_queue_t* queue);
void sched_block_locked(sched_wait_queue_t* queue, unsigned long flags);
void sched_wake_one(sched_wait_queue_t* queue);
void sched_wake_all(sched_wait_queue_t* queue);

void sched_tick(arch_int_state_t* state);
void sched_yield(void);
void sched_exit(void) NORETURN;
size_t sched_list_procs(proc_info_t* out, size_t capacity);
int sched_signal_send_pgrp(pid_t pgid, int signum);

bool sched_handle_cow_fault(sched_thread_t* thread, uintptr_t addr, bool write);
