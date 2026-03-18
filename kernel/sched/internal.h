#pragma once

#include "scheduler.h"
#include "mem.h"

#include <arch/mm.h>
#include <arch/paging.h>
#include <arch/thread.h>
#include <arch/signal.h>
#include <base/attributes.h>
#include <base/macros.h>
#include <base/units.h>
#include <data/hashmap.h>
#include <errno.h>
#include <inttypes.h>
#include <log/log.h>
#include <sched/signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cpu.h>
#include <sys/lock.h>
#include <sys/panic.h>
#include <sys/procfs.h>
#include <sys/tty.h>
#include <sys/wait.h>

#define SCHED_STACK_SIZE              (64 * KIB)
#define SCHED_LATENCY_NS              12000000ULL
#define SCHED_MIN_GRANULARITY_NS      1000000ULL
#define SCHED_REBALANCE_TICKS         32ULL
#define SCHED_RQ_CAPACITY             4096U
#define SCHED_SLEEP_HEAP_CAPACITY     SCHED_RQ_CAPACITY
#define SCHED_IDLE_STEAL_BATCH        2U
#define SCHED_PUSH_BATCH              2U
#define SCHED_EXIT_EVENT_CAP          512U
#define SCHED_WAKE_LOCAL_LOAD_SLOP    2U

typedef struct {
    spinlock_t lock;
    sched_thread_t **heap;
    u32 capacity;
    size_t nr_running;
    u64 min_vruntime ALIGNED(8);
} sched_rq_t;

typedef enum {
    SCHED_PID_IDLE = 0,
    SCHED_PID_USER,
    SCHED_PID_KERNEL,
} sched_pid_class_t;

typedef struct {
    sched_thread_t *current;
    sched_thread_t *idle_thread;
    sched_thread_t *handoff_ready;
    sched_thread_t *exit_pending;
    size_t preempt_depth;
    size_t sched_lock_depth;
    unsigned long sched_lock_irq_flags;
    u64 slice_ns ALIGNED(8);
    bool need_resched;
    bool force_resched;
    bool resched_irq_pending;
    u64 local_ticks ALIGNED(8);
} sched_cpu_state_t;

typedef struct {
    volatile u64 switch_count ALIGNED(8);
    volatile u64 syscall_count;
    volatile u64 migrations;
    volatile u64 steals;
    volatile u64 wake_ipi;
    volatile u64 runqueue_max;
    volatile u64 balance_runs;
    volatile u64 wait_timeout_count ALIGNED(8);
} sched_metrics_state_t;

typedef struct {
    volatile u64 busy_ticks ALIGNED(8);
    volatile u64 total_ticks;
    volatile u64 core_busy_ticks[MAX_CORES];
    volatile u64 core_total_ticks[MAX_CORES];
} sched_usage_state_t;

typedef struct {
    spinlock_t lock;
    ring_queue_t *ring;
} sched_exit_event_state_t;

typedef struct {
    sched_thread_t *heap[SCHED_SLEEP_HEAP_CAPACITY];
    size_t count;
    volatile u64 wake_tick ALIGNED(8);
} sched_sleep_state_t;

typedef struct {
    sched_rq_t runqueues[MAX_CORES];
    linked_list_t *zombie_list;
    linked_list_t *all_list;
    linked_list_t *deferred_destroy_list;
    hashmap_t *pid_index;
    arch_vm_space_t *kernel_vm;
    pid_t next_user_pid;
    pid_t next_kernel_pid;
    volatile u32 wake_rr_cursor;
    bool running;
    bool secondary_released;
    sched_cpu_state_t cpu[MAX_CORES];
    spinlock_t lock;
    sched_wait_queue_t poll_wait_queue;
    sched_wait_queue_t exit_event_wait;
    sched_wait_queue_t sleep_wait_queue;
    sched_exit_event_state_t exit_events;
    sched_sleep_state_t sleep;
    sched_usage_state_t usage;
    sched_metrics_state_t metrics;
} sched_state_t;


extern sched_state_t sched_state;

static inline bool thread_ctx_ok(const sched_thread_t *thread) {
    return thread && thread->context != 0;
}

static inline bool sched_thread_is_idle(const sched_thread_t *thread) {
    return thread && thread->pid == 0;
}

static inline bool sched_running_get(void) {
    return __atomic_load_n(&sched_state.running, __ATOMIC_ACQUIRE);
}

static inline void sched_running_set(bool running) {
    __atomic_store_n(&sched_state.running, running, __ATOMIC_RELEASE);
}

static inline bool sched_secondary_released_get(void) {
    return __atomic_load_n(&sched_state.secondary_released, __ATOMIC_ACQUIRE);
}

static inline void sched_secondary_released_set(bool released) {
    __atomic_store_n(&sched_state.secondary_released, released, __ATOMIC_RELEASE);
}

static inline u64 sched_ticks_to_ms(u64 ticks) {
    u64 hz = arch_timer_hz();
    return hz ? ((ticks * 1000ULL) / hz) : 0;
}

static inline size_t sched_cpu_id(void) {
    cpu_core_t *core = cpu_current();

    if (!core || core->id >= MAX_CORES) {
        if (core_count > 1) {
            panic("scheduler lost current core identity on SMP");
        }
        return 0;
    }

    return core->id;
}

static inline sched_cpu_state_t *sched_local(void) {
    return &sched_state.cpu[sched_cpu_id()];
}

static inline void sched_spin_wait(void) {
    if (arch_irq_enabled()) {
        arch_cpu_wait();
        return;
    }

    arch_cpu_relax();
}

static inline sched_thread_t *sched_local_current(void) {
    return __atomic_load_n(&sched_local()->current, __ATOMIC_ACQUIRE);
}

static inline void sched_local_set_current(sched_thread_t *thread) {
    __atomic_store_n(&sched_local()->current, thread, __ATOMIC_RELEASE);
}

static inline sched_thread_t *sched_local_idle(void) {
    return __atomic_load_n(&sched_local()->idle_thread, __ATOMIC_ACQUIRE);
}

static inline void sched_local_set_idle(sched_thread_t *thread) {
    __atomic_store_n(&sched_local()->idle_thread, thread, __ATOMIC_RELEASE);
}

static inline u64 sched_local_slice_ns(void) {
    return sched_local()->slice_ns;
}

static inline void sched_local_set_slice_ns(u64 slice_ns) {
    sched_local()->slice_ns = slice_ns;
}

static inline void sched_local_add_slice_ns(u64 delta_ns) {
    sched_local()->slice_ns += delta_ns;
}

static inline void sched_local_set_need_resched(bool need_resched) {
    __atomic_store_n(&sched_local()->need_resched, need_resched, __ATOMIC_RELEASE);
}

static inline bool sched_local_need_resched(void) {
    return __atomic_load_n(&sched_local()->need_resched, __ATOMIC_ACQUIRE);
}

static inline void sched_set_need_resched_cpu(size_t cpu_id, bool need_resched) {
    if (cpu_id >= MAX_CORES) {
        return;
    }

    __atomic_store_n(&sched_state.cpu[cpu_id].need_resched, need_resched, __ATOMIC_RELEASE);
}

static inline bool sched_mark_need_resched_cpu(size_t cpu_id) {
    if (cpu_id >= MAX_CORES) {
        return false;
    }

    bool prior = __atomic_exchange_n(
        &sched_state.cpu[cpu_id].need_resched, true, __ATOMIC_ACQ_REL
    );
    return !prior;
}

static inline void sched_local_inc_local_ticks(void) {
    sched_local()->local_ticks++;
}

static inline u64 sched_local_ticks(void) {
    return sched_local()->local_ticks;
}

static inline void sched_local_inc_preempt_depth(void) {
    sched_local()->preempt_depth++;
}

static inline void sched_local_dec_preempt_depth(void) {
    if (sched_local()->preempt_depth > 0) {
        sched_local()->preempt_depth--;
    }
}

static inline bool sched_local_preempt_disabled(void) {
    return sched_local()->preempt_depth != 0;
}

static inline u64 sched_tick_ns(void) {
    u64 hz = arch_timer_hz();
    if (!hz) {
        return 0;
    }

    u64 ns = 1000000000ULL / hz;
    return ns ? ns : 1ULL;
}

static inline u64 sched_online_cpu_mask(void) {
    u64 mask = 0;

    for (size_t i = 0; i < core_count && i < MAX_CORES && i < 64; i++) {
        if (cores_local[i].valid && cores_local[i].online) {
            mask |= 1ULL << i;
        }
    }

    if (!mask) {
        mask = 1ULL;
    }

    return mask;
}

static inline bool sched_cpu_allowed(const sched_thread_t *thread, size_t cpu_id) {
    if (!thread || cpu_id >= 64) {
        return false;
    }

    u64 mask = thread->allowed_cpu_mask;
    if (!mask) {
        mask = sched_online_cpu_mask();
    }

    return (mask & (1ULL << cpu_id)) != 0;
}

static inline thread_state_t thread_get_state(const sched_thread_t *thread) {
    if (!thread) {
        return THREAD_ZOMBIE;
    }

    return __atomic_load_n(&thread->state, __ATOMIC_ACQUIRE);
}

static inline void thread_set_state(
    sched_thread_t *thread, thread_state_t state
) {
    if (!thread) {
        return;
    }

    __atomic_store_n(&thread->state, state, __ATOMIC_RELEASE);
}

static inline int thread_cpu(const sched_thread_t *thread) {
    if (!thread) {
        return -1;
    }

    return __atomic_load_n(&thread->running_cpu, __ATOMIC_ACQUIRE);
}

static inline void thread_set_cpu(
    sched_thread_t *thread, int cpu_id
) {
    if (!thread) {
        return;
    }

    __atomic_store_n(&thread->running_cpu, cpu_id, __ATOMIC_RELEASE);
}

static inline void thread_unclaim(sched_thread_t *thread) {
    thread_set_cpu(thread, -1);
}

static inline void thread_claim(sched_thread_t *thread, size_t cpu_id) {
    if (!thread) {
        return;
    }

    thread_set_cpu(thread, (int)cpu_id);
    thread_set_state(thread, THREAD_RUNNING);
}

static inline bool thread_on_local_cpu(const sched_thread_t *thread) {
    int running_cpu = thread_cpu(thread);
    if (!thread || running_cpu < 0 || (size_t)running_cpu >= MAX_CORES) {
        return false;
    }

    size_t cpu_id = (size_t)running_cpu;
    sched_thread_t *current = __atomic_load_n(
        &sched_state.cpu[cpu_id].current,
        __ATOMIC_ACQUIRE
    );
    return current == thread;
}

static inline bool thread_in_handoff(const sched_thread_t *thread) {
    int running_cpu = thread_cpu(thread);

    if (!thread || running_cpu < 0 || (size_t)running_cpu >= MAX_CORES) {
        return false;
    }

    size_t cpu_id = (size_t)running_cpu;
    sched_thread_t *handoff = __atomic_load_n(
        &sched_state.cpu[cpu_id].handoff_ready,
        __ATOMIC_ACQUIRE
    );

    return handoff == thread;
}

static inline bool thread_is_owned(const sched_thread_t *thread) {
    return thread_on_local_cpu(thread) || thread_in_handoff(thread);
}

static inline bool
sched_reclaim_handoff(sched_thread_t *thread) {
    int running_cpu = thread_cpu(thread);

    if (!thread || running_cpu < 0 || (size_t)running_cpu >= MAX_CORES) {
        return false;
    }

    size_t cpu_id = (size_t)running_cpu;
    sched_thread_t *handoff = __atomic_load_n(
        &sched_state.cpu[cpu_id].handoff_ready,
        __ATOMIC_ACQUIRE
    );

    if (handoff != thread) {
        return false;
    }

    __atomic_store_n(&sched_state.cpu[cpu_id].handoff_ready, NULL, __ATOMIC_RELEASE);
    thread_unclaim(thread);

    return true;
}

void sched_nudge_thread(sched_thread_t *thread);
void enqueue_ipi(sched_thread_t *thread, bool allow_remote_ipi);

static inline bool
sched_repair_thread(
    sched_thread_t *thread,
    bool send_ipi
) {
    if (!thread) {
        return false;
    }

    if (thread_get_state(thread) != THREAD_RUNNING) {
        return false;
    }

    if (sched_reclaim_handoff(thread)) {
        thread_set_state(thread, THREAD_READY);
        enqueue_ipi(thread, send_ipi);
        return true;
    }

    if (thread_on_local_cpu(thread)) {
        sched_nudge_thread(thread);
        return true;
    }

    if (thread_in_handoff(thread)) {
        return true;
    }

    thread_unclaim(thread);
    thread_set_state(thread, THREAD_READY);
    enqueue_ipi(thread, send_ipi);
    return true;
}

unsigned long sched_lock_save(void);
bool sched_lock_try_save(unsigned long *flags_out);
void sched_lock_restore(unsigned long flags);
bool sched_reconcile_lock(void);

bool sleep_heap_insert(sched_thread_t *thread);
void sleep_heap_remove_at(size_t index);
void sleep_heap_remove(sched_thread_t *thread);
sched_thread_t *sleep_heap_top(void);
pid_t sched_next_pid(sched_pid_class_t pid_class);

bool ctx_candidate_valid(
    const sched_thread_t *thread,
    const arch_int_state_t *state
);
bool ctx_valid(const sched_thread_t *thread);

size_t sched_cpu_load(size_t cpu_id);
size_t sched_rq_depth(size_t cpu_id);
bool cpu_needs_ipi(size_t cpu_id);
size_t sched_cpu_distance(size_t from_cpu, size_t to_cpu, size_t cpu_count);
size_t pick_cpu(const sched_thread_t *thread, size_t disallowed_cpu);
void sched_publish_handoff(sched_thread_t *thread, size_t cpu_id);
void sched_flush_handoff(size_t cpu_id);
void rq_note_depth(size_t depth);
void rq_enqueue_cpu(sched_thread_t *thread, size_t cpu_id);
bool rq_remove_thread(sched_thread_t *thread);
bool rq_remove_index(sched_rq_t *rq, u32 index);
sched_thread_t *rq_peek_best(size_t cpu_id);
sched_thread_t *rq_pop_best_allowed(size_t cpu_id);
sched_thread_t *rq_pop_disallowed_from_cpu(size_t cpu_id, size_t allowed_cpu);
sched_thread_t *rq_pop_worst_allowed_from_cpu(size_t cpu_id, size_t allowed_cpu);

u64 sched_target_slice_ns(size_t cpu_id);
bool sched_has_better_runnable(sched_thread_t *current, size_t cpu_id);
void sched_rebalance_once(size_t cpu_id);

bool wake_cpu(size_t cpu_id);
void enqueue_thread(sched_thread_t *thread);
void rq_remove(sched_thread_t *thread);
sched_thread_t *dequeue_thread(void);
sched_thread_t *pick_next_thread(void);
sched_thread_t *pick_init_thread(void);

u64 _pid_index_key(pid_t pid);
sched_thread_t *pid_get(pid_t pid);
void pid_set(sched_thread_t *thread);
void pid_remove(pid_t pid);
void thread_add(sched_thread_t *thread);
bool sched_fd_refs_node(const vfs_node_t *node);
void thread_set_name(sched_thread_t *thread, const char *name);
void thread_cleanup(sched_thread_t *thread);
sched_thread_t *find_thread(pid_t pid);
void thread_get(sched_thread_t *thread);
void thread_put(sched_thread_t *thread);
void thread_destroy(sched_thread_t *thread);
void sched_reap_deferred(void);
void sched_reap(void);
NORETURN void thread_trampoline(void);
void idle_entry(void *arg);
void thread_prepare_user(
    sched_thread_t *thread,
    uintptr_t entry,
    uintptr_t user_stack_top
);
sched_thread_t *create_thread(
    const char *name,
    thread_entry_t entry,
    void *arg,
    bool enqueue,
    bool user_thread,
    sched_pid_class_t pid_class
);

void wake_sleepers(u64 now);
void sched_wake_sleepers(u64 now);
void wq_dequeue(sched_thread_t *thread);
void wq_remove(sched_thread_t *thread);
bool wait_running(sched_thread_t *self);
void exit_event_push(pid_t pid);

void sched_capture_context(arch_int_state_t *state);
void force_resched(void);
