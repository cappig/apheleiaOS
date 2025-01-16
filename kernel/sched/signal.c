#include "signal.h"

#include <base/addr.h>
#include <base/types.h>
#include <log/log.h>
#include <string.h>

#include "aos/signals.h"
#include "arch/gdt.h"
#include "arch/idt.h"
#include "sched/process.h"
#include "sched/scheduler.h"
#include "sys/panic.h"

// What if the signal handler is at address 0x0 of 0x2, it is possible?
#define SIGNAL_TERMINATE ((sighandler_fn)0)
#define SIGNAL_IGNORE    ((sighandler_fn)1)
// TODO: core dump

static sighandler_fn _default_actions[SIGNAL_COUNT] = {
    [0] = SIGNAL_TERMINATE,

    [SIGCHLD] = SIGNAL_IGNORE,

    [SIGUSR1] = SIGNAL_TERMINATE,
    [SIGUSR2] = SIGNAL_TERMINATE,

    [SIGINT] = SIGNAL_TERMINATE,
    [SIGHUP] = SIGNAL_TERMINATE,
    [SIGTRAP] = SIGNAL_TERMINATE,

    [SIGTERM] = SIGNAL_TERMINATE,
    [SIGABRT] = SIGNAL_TERMINATE,

    [SIGSYS] = SIGNAL_TERMINATE,
    [SIGFPE] = SIGNAL_TERMINATE,
    [SIGILL] = SIGNAL_TERMINATE,
    [SIGBUS] = SIGNAL_TERMINATE,
    [SIGSEGV] = SIGNAL_TERMINATE,

    [SIGKILL] = SIGNAL_TERMINATE,
};


static inline u16 _mask_pending(process* proc) {
    u16 mask = ~proc->user.signals.masked;
    mask |= 1 << (SIGKILL - 1); // SIGKILL can't be masked

    return proc->user.signals.pending & mask;
}

static bool _mark_signal(process* proc, usize signum) {
    if (signum >= SIGNAL_COUNT || !signum)
        return false;

    u16 mask = 1 << (signum - 1);
    proc->user.signals.pending |= mask;

    return true;
}

static bool _unmark_signal(process* proc, usize signum) {
    if (signum >= SIGNAL_COUNT || !signum)
        return false;

    u16 mask = 1 << (signum - 1);
    proc->user.signals.pending &= ~mask;

    return true;
}

// NOTE: rsp _has to be_ updated after this function gets called
// This assumes that the stack ptr is valid
static void _push_to_ustack(process* proc, u64 rsp, sig_state* state) {
    rsp -= sizeof(sig_state);

    usize offset = rsp - proc->user.stack;
    u64 paddr = proc->user.stack_paddr + offset;
    void* vaddr = (void*)ID_MAPPED_VADDR(paddr);

    memcpy(vaddr, state, sizeof(sig_state));
}


bool process_signal_defaults(process* proc) {
    if (proc->type != PROC_USER)
        return false;

    memcpy(proc->user.signals.handlers, _default_actions, sizeof(_default_actions));
    return true;
}

bool signal_set_handler(process* proc, usize signum, sighandler_fn handler) {
    if (signum >= SIGNAL_COUNT)
        return false;

    if (signum >= SIGNAL_COUNT || !signum)
        return false;

    if (handler == SIG_DFL)
        handler = _default_actions[signum - 1];

    proc->user.signals.handlers[signum - 1] = handler;

    return true;
}

// Get the next handleable signal number, 0 if none
usize signal_get_pending(process* proc) {
    if (proc->type != PROC_USER)
        return 0;

    // Ignore masked signals
    u16 pending = _mask_pending(proc);

    if (!pending)
        return 0;

    usize signum = 0;

    // Find the highest priority pending signal
    while (pending >>= 1)
        signum++;

    signum += 1;

    // So we can interrupt other signal handlers but only if the
    // pending signal is of a higher priority. This means that signals
    // can nest and the highest possible nest depth is SIGNAL_COUNT-1
    if (signum <= proc->user.signals.current)
        return 0;

    if (proc->user.signals.current)
        _mark_signal(proc, proc->user.signals.current);

    proc->user.signals.current = signum;

    _unmark_signal(proc, signum);

    return signum;
}

// Replace process context with the signal handler or terminate
// Returns false if the process terminated and true if it didn't
// It is expected that SIGKILL gets handled before this function ever gets called
void prepare_signal(process* proc, usize signum) {
    assert(proc->type == PROC_USER);

    if (signum >= SIGNAL_COUNT || !signum)
        return;

    int_state* current = (int_state*)proc->stack_ptr;

    sighandler_fn handler = proc->user.signals.handlers[signum - 1];

    // TODO: allow for separate signal handler stacks
    u64 old_rsp = current->s_regs.rsp - 128; // Assume that we have a 128 byte red zone
    u64 new_rsp = old_rsp - sizeof(sig_state);

    // Fall back to default actions if something goes wrong
    if (!process_validate_ptr(proc, (void*)new_rsp, sizeof(sig_state), true))
        handler = _default_actions[signum]; // Should this always kill the process?

    if (!proc->user.signals.trampoline)
        handler = _default_actions[signum];

    if (handler == SIGNAL_IGNORE)
        return;

    if (handler == SIGNAL_TERMINATE) {
        scheduler_kill(proc, EXIT_SIGNAL_BASE + signum);
        return;
    }

    assert(proc->user.stack);

    sig_state state = {
        .ret = (u64)proc->user.signals.trampoline,

        .g_regs = current->g_regs,

        .magic = SIGNAL_MAGIC,
        .signal = signum,

        .rip = current->s_regs.rip,
        .rflags = current->s_regs.rflags,
    };

    _push_to_ustack(proc, old_rsp, &state);

    int_state pstate = {
        .s_regs.rip = (u64)handler,
        .s_regs.cs = GDT_user_code | 3,
        .s_regs.rflags = 0x200,
        .s_regs.rsp = new_rsp,
        .s_regs.ss = GDT_user_data | 3,

        // Pass the argument to the handler function (sysV abi)
        .g_regs.rdi = signum,
    };

    process_set_state(proc, &pstate);

    return;
}

// Restore the state prior to the signal getting scheduled
// Due to the way this kernel performs task switching a signal
// will never interrupt kernel work (syscalls etc.). This also
// means that processes with pending signals only get to handle
// them once they get the next timeslice
bool signal_return(process* proc) {
    if (!proc->user.signals.current)
        goto fail;

    int_state* current = (int_state*)proc->stack_ptr;

    // Locate the old state on the userspace stack
    // Compensate for the popped return value
    u64 rsp = current->s_regs.rsp - sizeof(u64);
    usize offset = rsp - proc->user.stack;

    u64 paddr = proc->user.stack_paddr + offset;
    void* vaddr = (void*)ID_MAPPED_VADDR(paddr);

    void* ptr = (void*)proc->user.stack + offset;

    if (!process_validate_ptr(proc, ptr, sizeof(sig_state), false))
        goto fail;

    sig_state* sstate = (sig_state*)vaddr;

    if (sstate->magic != SIGNAL_MAGIC || sstate->signal != proc->user.signals.current)
        goto fail;

    int_state pstate = {
        .g_regs = sstate->g_regs,

        .s_regs.rip = sstate->rip,
        .s_regs.cs = GDT_user_code | 3,
        .s_regs.rflags = sstate->rflags,
        .s_regs.rsp = rsp + sizeof(sig_state) + 128,
        .s_regs.ss = GDT_user_data | 3,

        .int_num = 0x80,
    };

    process_set_state(proc, &pstate);

    proc->user.signals.current = 0;

    return true;

    // Calling sigretrun with invalid state on the stack is fatal
fail:
    log_error("Invalid sigreturn (pid %zu)! Terminating!", proc->id);
    signal_send(proc, SIGKILL);
    return false;
}


void signal_send(process* proc, usize signum) {
    if (!proc || !signum)
        return;

    if (signum == SIGKILL)
        scheduler_kill(proc, EXIT_SIGNAL_BASE + SIGKILL);

#ifdef SCHED_DEBUG
    log_debug("[SCHED_DEBUG] pid %zu has new signal %zu", proc->id, signum);
#endif

    _mark_signal(proc, signum);
}
