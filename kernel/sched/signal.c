#include "signal.h"

#include <base/types.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "log/log.h"
#include "sched/process.h"
#include "sched/scheduler.h"

typedef struct {
    pid_t group;
    int signum;
} sig_group_data;

// TODO: core dumps
static sighandler_t default_actions[NSIG + 1] = {
    [0] = SIG_DFL,

    [SIGCHLD] = SIG_IGN,

    [SIGUSR1] = SIG_DFL,
    [SIGUSR2] = SIG_DFL,

    [SIGINT] = SIG_DFL,
    [SIGHUP] = SIG_DFL,
    [SIGTRAP] = SIG_DFL,

    [SIGTERM] = SIG_DFL,
    [SIGABRT] = SIG_DFL,

    [SIGSYS] = SIG_DFL,
    [SIGFPE] = SIG_DFL,
    [SIGILL] = SIG_DFL,
    [SIGBUS] = SIG_DFL,
    [SIGSEGV] = SIG_DFL,

    [SIGKILL] = SIG_DFL,
};


static inline void _mark_signal(sched_process* proc, usize signum) {
    u16 mask = 1 << (signum - 1);
    proc->signals.pending |= mask;
}

static inline void _unmark_signal(sched_process* proc, usize signum) {
    u16 mask = 1 << (signum - 1);
    proc->signals.pending &= ~mask;
}

// Here SIG_DFL return value will always mean 'terminate the process'
static sighandler_t _get_handler(sched_process* proc, usize signum) {
    if (!proc->memory.trampoline)
        return SIG_DFL;

    if (signum == SIGKILL) // TODO: SIGSTOP
        return SIG_DFL;

    sighandler_t handler = proc->signals.handlers[signum];

    if (handler == SIG_DFL)
        return default_actions[signum];

    return handler;
}

static inline u32 _mask_pending(sched_thread* thread) {
    u32 mask = ~thread->signal_mask;
    mask |= 1 << (SIGKILL - 1); // SIGKILL can't be masked

    return thread->proc->signals.pending & mask;
}

static inline bool _is_masked(sched_thread* thread, usize signum) {
    u32 mask = 1 << (signum - 1);
    return thread->signal_mask & mask;
}


static void _push_to_ustack(sched_thread* thread, u64 rsp, sig_state* state) {
    rsp -= sizeof(sig_state);

    usize offset = rsp - thread->ustack.vaddr;
    u64 paddr = thread->ustack.paddr + offset;
    u64* vaddr = (void*)ID_MAPPED_VADDR(paddr);

    memcpy(vaddr, state, sizeof(sig_state));
}

// Called before a context switch to handle a signal with a registered handler
void thread_signal_switch(sched_thread* thread, usize signum) {
    sched_process* proc = thread->proc;

    int_state* current = (int_state*)thread->kstack.ptr;
    sighandler_t handler = proc->signals.handlers[signum];

    // TODO: separate signal handler stacks
    u64 old_rsp = current->s_regs.rsp - 128; // Assume that we have a 128 byte red zone
    u64 new_rsp = old_rsp - sizeof(sig_state);

    sig_state state = {
        .ret = (u64)proc->memory.trampoline,

        .g_regs = current->g_regs,

        .magic = SIGNAL_MAGIC,
        .signal = signum,

        .rip = current->s_regs.rip,
        .rflags = current->s_regs.rflags,
    };

    _push_to_ustack(thread, old_rsp, &state);

    int_state tstate = {
        .s_regs.rip = (u64)handler,
        .s_regs.cs = GDT_user_code | 3,
        .s_regs.rflags = 0x200,
        .s_regs.rsp = new_rsp,
        .s_regs.ss = GDT_user_data | 3,

        // Pass the argument to the handler function (sysV abi)
        .g_regs.rdi = signum,
    };

    _unmark_signal(proc, signum);
    thread_set_state(thread, &tstate);

    thread->current_signal = signum;
}

// Restore the state prior to the signal getting scheduled
bool thread_signal_return(sched_thread* thread) {
    sched_process* proc = thread->proc;

    /* if (!thread->current_signal) */
    /*     return false; */
    // TODO: signal nesting ??

    int_state* current = (int_state*)thread->kstack.ptr;

    // Locate the old state on the userspace stack
    // Compensate for the popped return value
    u64 rsp = current->s_regs.rsp - sizeof(u64);
    usize offset = rsp - thread->ustack.vaddr;

    u64 paddr = thread->ustack.paddr + offset;
    void* vaddr = (void*)ID_MAPPED_VADDR(paddr);

    void* ptr = (void*)thread->ustack.vaddr + offset;

    if (!proc_validate_ptr(proc, ptr, sizeof(sig_state), false))
        return false;

    sig_state* sstate = (sig_state*)vaddr;

    if (sstate->magic != SIGNAL_MAGIC)
        return false;

    int_state tstate = {
        .g_regs = sstate->g_regs,

        .s_regs.rip = sstate->rip,
        .s_regs.cs = GDT_user_code | 3,
        .s_regs.rflags = sstate->rflags,
        .s_regs.rsp = rsp + sizeof(sig_state) + 128, // the red zone is 128 bytes
        .s_regs.ss = GDT_user_data | 3,

        .int_num = 0x80,
    };

    thread_set_state(thread, &tstate);

    thread->current_signal = 0;

    return true;
}


usize thread_signal_get_pending(sched_thread* thread) {
    u32 pending = _mask_pending(thread);

    if (!pending)
        return 0;

    usize signum = 0;

    // Find the highest priority pending signal
    while (pending >>= 1)
        signum++;

    signum += 1;

    // We can interrupt other signal handlers but only if the
    // pending signal is of a higher priority. This means that signals
    // can nest and the highest possible nest depth is SIGNAL_COUNT-1
    /* if (signum <= thread->current_signal) */
    /*     return 0; */
    // TODO: this ^

    return signum;
}


// Send a signal to a process, if tid is -1 than the signal can be delivered to any
// thread in the process, otherwise the signal gets directed to the specified thread
// Retruns -1 on error, 0 if the signal got ignored, 1 if the process got killed
// right away or 2 if it got marked as pending
int signal_send(sched_process* proc, tid_t tid, usize signum) {
    if (!signum)
        return -1;

    // Signal dispostions are set per process not per thread.
    // This means that all threads handle all signals in the same way
    sighandler_t handler = _get_handler(proc, signum);

    if (handler == SIG_IGN)
        return 0;

    // init can only receive signals for which it has explicitly installed handlers
    if (proc->pid == INIT_PID && !handler)
        return 0;

    if (handler == SIG_DFL) {
        proc_terminate(proc, EXIT_SIGNAL_BASE + signum);
        return 1;
    }

    // If the signal is directed attempt to prepare the thread
    if (tid >= 0) {
        sched_thread* thread = proc_get_thread(proc, tid);

        // Note that this means that the user can just mask/ignore fatal signals like SIGSEGV,
        // this will put the thread into an infinite loop of attempting to rerun the offendig
        // instruction and SEGFAULTing. We would have to send SIGKILL to stop this
        if (_is_masked(thread, signum))
            return 0;

        thread_signal_switch(thread, signum);
    } else {
        // No particular thread is targeted, deliver to the first one that is able to handle it
        _mark_signal(proc, signum);
    }

    return 2;
}

int signal_send_pid(pid_t pid, tid_t tid, usize signum) {
    sched_process* proc = sched_get_proc(pid);

    if (!proc)
        return -1;

    return signal_send(proc, tid, signum);
}


static bool _send_group(const void* data, void* private) {
    sched_process* proc = (sched_process*)data;
    sig_group_data* gdata = private;

    if (proc->group == gdata->group)
        signal_send(proc, -1, gdata->signum);

    return 0;
}

int signal_send_group(pid_t group, usize signum) {
    if (!group)
        return -1;

    sig_group_data data = {group, signum};

    tree_foreach(proc_tree, _send_group, &data);

    return 2;
}


bool proc_signal_set_handler(sched_process* proc, usize signum, sighandler_t handler) {
    if (signum >= NSIG || !signum)
        return false;

    if (signum == SIGKILL)
        return false;

    proc->signals.handlers[signum] = handler;

    return true;
}
