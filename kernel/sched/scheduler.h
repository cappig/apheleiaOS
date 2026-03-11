#pragma once

#include <arch/arch.h>
#include <arch/context.h>
#include <base/attributes.h>
#include <base/types.h>
#include <data/list.h>
#include <data/ring.h>
#include <limits.h>
#include <signal.h>
#include <sys/proc.h>
#include <sys/types.h>
#include <sys/lock.h>

typedef void (*thread_entry_t)(void *arg);

typedef struct vfs_node vfs_node_t;

#define SCHED_FD_MAX     256
#define SCHED_REGION_COW (1ULL << 62)
#define SCHED_GROUP_MAX  16

#define SCHED_PIPE_CAPACITY 4096
#define SCHED_FD_FLAG_CLOEXEC (1u << 0)

typedef enum {
    SCHED_FD_NONE = 0,
    SCHED_FD_VFS,
    SCHED_FD_PIPE_READ,
    SCHED_FD_PIPE_WRITE,
} sched_fd_kind_t;

typedef struct sched_pipe {
    spinlock_t lock;
    ring_io_t ring;
    size_t readers;
    size_t writers;
    size_t wake_refs;
    bool destroying;
    struct sched_wait_queue *read_wait_queue;
    struct sched_wait_queue *write_wait_queue;
    bool read_wait_owned;
    bool write_wait_owned;
} sched_pipe_t;

typedef struct sched_fd {
    sched_fd_kind_t kind;
    vfs_node_t *node;
    sched_pipe_t *pipe;
    size_t offset;
    int pty_index;
    int tty_index;
    u32 flags;
    u32 fd_flags;
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
    struct sched_user_region *next;
} sched_user_region_t;

typedef struct sched_wait_queue {
    linked_list_t *list;
    spinlock_t lock;
    const char *debug_name;
    u32 wake_seq;
    u32 waiter_count;
    bool poll_link;
} sched_wait_queue_t;

typedef enum {
    SCHED_WAIT_WOKEN = 0,
    SCHED_WAIT_TIMEOUT = 1,
    SCHED_WAIT_INTR = 2,
    SCHED_WAIT_ABORTED = 3,
} sched_wait_result_t;

typedef u32 sched_wait_flags_t;

#define SCHED_WAIT_INTERRUPTIBLE (1U << 0)
#define SCHED_WAIT_POLL_LINK     (1U << 1)

typedef struct sched_thread {
    char name[PROC_NAME_MAX];
    thread_state_t state;
    size_t affinity_core;
    size_t last_cpu;
    u64 allowed_cpu_mask ALIGNED(8);
    bool affinity_user_set;

    u64 vruntime_ns ALIGNED(8);
    u64 exec_start_ns ALIGNED(8);
    u64 sum_exec_ns ALIGNED(8);

    list_node_t run_node;
    bool in_run_queue;
    bool on_rq;
    u32 rq_index;

    list_node_t wait_node;
    bool in_wait_queue;
    struct sched_wait_queue *blocked_on;

    list_node_t zombie_node;
    bool in_zombie_list;

    void *stack;
    size_t stack_size;
    uintptr_t context;

    thread_entry_t entry;
    void *arg;

    bool user_thread;
    pid_t pid;
    pid_t ppid;
    pid_t pgid;
    pid_t sid;
    uid_t uid;
    gid_t gid;
    gid_t groups[SCHED_GROUP_MAX];
    size_t group_count;
    mode_t umask;
    int exit_code;

    arch_vm_space_t *vm_space;
    spinlock_t vm_lock;

    uintptr_t user_stack_base;
    size_t user_stack_size;
    u64 user_mem_kib ALIGNED(8);

    struct sched_user_region *regions;

    sched_wait_queue_t wait_queue;

    list_node_t all_node;
    bool in_all_list;

    volatile u32 refcount;
    u32 lifecycle_flags;
    list_node_t deferred_node;
    bool in_deferred_list;

    sched_fd_t fds[SCHED_FD_MAX];
    bool fd_used[SCHED_FD_MAX];

    char cwd[PATH_MAX];

    u64 wake_tick ALIGNED(8);
    bool sleep_queued;
    size_t sleep_index;
    u64 wait_deadline_tick ALIGNED(8);
    u32 wait_flags;
    u8 wait_result;
    u8 wait_cookie;

    u32 signal_pending;
    u32 signal_mask;
    volatile u32 current_signal;
    int stop_signal;
    bool stop_reported;
    volatile u32 signal_saved_valid;
    uintptr_t signal_trampoline;
    arch_int_state_t signal_saved_state;
    sighandler_t signal_handlers[NSIG];

    int tty_index;
    u64 cpu_time_ticks ALIGNED(8);
    int running_cpu;

    u8 fpu_state[512] ALIGNED(16);
    bool fpu_initialized;
} sched_thread_t;

typedef struct {
    u64 sched_switch_count;
    u64 sched_migrations;
    u64 sched_steals;
    u64 sched_wake_ipi;
    u64 sched_runqueue_max;
    u64 sched_balance_runs;
    u64 sched_ownership_conflicts;
    u64 sched_ref_underflow;
    u64 wait_timeout_count;
    u64 sched_lock_contention_spins;
    u64 lockdep_inversion_count;
    u64 lockdep_block_under_spin_count;
} sched_metrics_snapshot_t;

enum {
    SCHED_THREAD_LIFECYCLE_DEFER_QUEUED = 1U << 0,
    SCHED_THREAD_LIFECYCLE_DESTROYING = 1U << 1,
};

typedef struct {
    pid_t pid;
    pid_t ppid;
    pid_t pgid;
    pid_t sid;
    uid_t uid;
    gid_t gid;
    mode_t umask;
    u32 signal_pending;
    u32 signal_mask;
    thread_state_t state;
    int core_id;
    int tty_index;
    u64 cpu_time_ms;
    u64 vm_kib;
    char name[PROC_NAME_MAX];
} sched_proc_snapshot_t;


void scheduler_init(void);
void scheduler_init_core(void);
void scheduler_start(void);
void scheduler_start_secondary(void);

bool sched_is_running(void);

sched_thread_t *sched_current(void);
sched_thread_t *sched_current_core(size_t core_id);
sched_thread_t *sched_find_thread(pid_t pid);
bool sched_pid_alive(pid_t pid);
void sched_thread_get(sched_thread_t *thread);
void sched_thread_put(sched_thread_t *thread);
int sched_set_affinity(pid_t pid, u64 mask);
int sched_get_affinity(pid_t pid, u64 *mask_out);
pid_t sched_getpid(void);
pid_t sched_getppid(void);
uid_t sched_getuid(void);
gid_t sched_getgid(void);
mode_t sched_getumask(void);
int sched_setuid(uid_t uid);
int sched_setgid(gid_t gid);
int sched_setgroups(const gid_t *groups, size_t group_count);
int sched_setumask(mode_t mask);
int sched_getgroups(gid_t *groups, size_t max_groups, size_t *group_count_out);
bool sched_gid_matches_cred(uid_t uid, gid_t gid, gid_t target_gid);
int sched_getgroups_pid(
    pid_t pid,
    gid_t *primary_gid_out,
    gid_t *groups_out,
    size_t max_groups,
    size_t *group_count_out
);
pid_t sched_getpgid(pid_t pid);
int sched_setpgid(pid_t pid, pid_t pgid);
pid_t sched_setsid(void);
bool sched_process_is_child(pid_t child_pid, pid_t parent_pid);
bool sched_pid_is_group_leader(pid_t pid);
bool sched_pgrp_exists(pid_t pgid);
bool sched_pgrp_in_session(pid_t pgid, pid_t sid);
sched_thread_t *
sched_create_kernel_thread(const char *name, thread_entry_t entry, void *arg);
sched_thread_t *sched_create_user_thread(const char *name);
pid_t sched_fork(arch_int_state_t *state);
pid_t sched_wait(pid_t pid, int *status);
pid_t sched_waitpid(pid_t pid, int *status, int options);
void sched_prepare_user_thread(
    sched_thread_t *thread,
    uintptr_t entry,
    uintptr_t user_stack_top
);
void sched_discard_thread(sched_thread_t *thread);
void sched_make_runnable(sched_thread_t *thread);
void sched_unblock_thread(sched_thread_t *thread);
void sched_stop_thread(sched_thread_t *thread, int signum);
void sched_continue_thread(sched_thread_t *thread);
void sched_set_thread_name(sched_thread_t *thread, const char *name);

bool sched_add_user_region(
    sched_thread_t *thread,
    uintptr_t vaddr,
    uintptr_t paddr,
    size_t pages,
    u64 flags
);
void sched_clear_user_regions(sched_thread_t *thread);
void sched_user_mem_add(sched_thread_t *thread, size_t pages);
void sched_user_mem_sub(sched_thread_t *thread, size_t pages);
void sched_user_mem_set_kib(sched_thread_t *thread, u64 kib);
u64 sched_user_mem_kib(const sched_thread_t *thread);

void sched_wait_queue_init(sched_wait_queue_t *queue);
void sched_wait_queue_destroy(sched_wait_queue_t *queue);
void sched_wait_queue_set_name(sched_wait_queue_t *queue, const char *name);
void sched_wait_queue_set_poll_link(sched_wait_queue_t *queue, bool enabled);

u32 sched_wait_seq(sched_wait_queue_t *queue);
sched_wait_result_t sched_wait_on_queue(
    sched_wait_queue_t *queue,
    u32 observed_seq,
    u64 deadline_tick,
    sched_wait_flags_t flags
);
bool sched_block_if_unchanged(sched_wait_queue_t *queue, u32 observed_seq);
void sched_block(sched_wait_queue_t *queue);
void sched_wake_one(sched_wait_queue_t *queue);
void sched_wake_all(sched_wait_queue_t *queue);
u32 sched_poll_wait_seq(void);
bool sched_poll_block_if_unchanged(u32 observed_seq);
bool sched_poll_block_if_unchanged_until(u32 observed_seq, u64 deadline_tick);
void sched_poll_wait(void);
sched_wait_result_t
sched_wait_deadline(u64 deadline_tick, sched_wait_flags_t flags);
u32 sched_exit_event_seq(void);
bool sched_exit_event_block_if_unchanged(u32 observed_seq);
bool sched_exit_event_pop(pid_t *pid_out);

void sched_preempt_disable(void);
void sched_preempt_enable(void);
bool sched_preempt_disabled(void);

void sched_tick(arch_int_state_t *state);
void sched_yield(void);
void sched_sleep(u64 ticks);
void sched_capture_context(arch_int_state_t *state);
void sched_ipi_resched(void);
void sched_resched_softirq(arch_int_state_t *state);
void sched_request_resched_local_force(void);
void sched_request_resched_local(void);
void sched_lockdep_note_block_under_spin(void);
bool sched_wait_until_running(sched_thread_t *self);
void sched_exit(void) NORETURN;
bool sched_proc_snapshot(pid_t pid, sched_proc_snapshot_t *out);
void sched_cpu_usage_snapshot(u64 *busy_ticks_out, u64 *total_ticks_out);
void sched_cpu_usage_snapshot_core(
    size_t core_id,
    u64 *busy_ticks_out,
    u64 *total_ticks_out
);
void sched_metrics_snapshot(sched_metrics_snapshot_t *out);
int sched_signal_send_pgrp(pid_t pgid, int signum);

bool sched_handle_cow_fault(sched_thread_t *thread, uintptr_t addr, bool write);
bool sched_proc_cwd(pid_t pid, char *out, size_t out_len);

int sched_fd_alloc(sched_thread_t *thread, const sched_fd_t *fd, int min_fd);
int sched_fd_install(
    sched_thread_t *thread,
    int target_fd,
    const sched_fd_t *fd
);
int sched_fd_close(sched_thread_t *thread, int fd);
int sched_fd_dup(sched_thread_t *thread, int oldfd, int newfd);
bool sched_fd_clone_table(sched_thread_t *dst, const sched_thread_t *src);
void sched_fd_close_all(sched_thread_t *thread);
void sched_fd_close_cloexec(sched_thread_t *thread);
bool sched_fd_refs_node(const vfs_node_t *node);

sched_pipe_t *sched_pipe_create(size_t capacity);
void sched_pipe_acquire_reader(sched_pipe_t *pipe);
void sched_pipe_acquire_writer(sched_pipe_t *pipe);
void sched_pipe_release_reader(sched_pipe_t *pipe);
void sched_pipe_release_writer(sched_pipe_t *pipe);
bool sched_pipe_operation_begin(sched_pipe_t *pipe);
void sched_pipe_operation_end(sched_pipe_t *pipe);
