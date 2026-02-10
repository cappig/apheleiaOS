#include "signal.h"

#include <arch/signal.h>
#include <sched/scheduler.h>
#include <string.h>

static const sighandler_t default_actions[NSIG] = {
    [SIGCHLD] = SIG_IGN,
};

static bool _valid(int signum) {
    return signum > 0 && signum < NSIG;
}

static sighandler_t _get_handler(sched_thread_t* thread, int signum) {
    if (!thread || !_valid(signum))
        return SIG_DFL;

    if (signum == SIGKILL)
        return SIG_DFL;

    sighandler_t handler = thread->signal_handlers[signum];
    if (handler == SIG_DFL) {
        sighandler_t def = default_actions[signum];
        return def ? def : SIG_DFL;
    }

    return handler;
}

static void _mark_pending(sched_thread_t* thread, int signum) {
    if (!thread || !_valid(signum))
        return;

    u32 mask = 1u << (signum - 1);
    thread->signal_pending |= mask;
}

static void _clear_pending(sched_thread_t* thread, int signum) {
    if (!thread || !_valid(signum))
        return;

    u32 mask = 1u << (signum - 1);
    thread->signal_pending &= ~mask;
}

static int _next_pending(sched_thread_t* thread) {
    if (!thread)
        return 0;

    u32 pending = thread->signal_pending;
    if (!pending)
        return 0;

    for (int signum = 1; signum < NSIG; signum++) {
        if (pending & (1u << (signum - 1)))
            return signum;
    }

    return 0;
}

void sched_signal_init_thread(sched_thread_t* thread) {
    if (!thread)
        return;

    memset(thread->signal_handlers, 0, sizeof(thread->signal_handlers));
    thread->signal_pending = 0;
    thread->signal_mask = 0;
    thread->current_signal = 0;
    thread->signal_saved_valid = false;
    thread->signal_trampoline = 0;
}

void sched_signal_reset_thread(sched_thread_t* thread) {
    sched_signal_init_thread(thread);
}

sighandler_t sched_signal_set_handler(
    sched_thread_t* thread,
    int signum,
    sighandler_t handler,
    uintptr_t trampoline
) {
    if (!thread || !_valid(signum))
        return SIG_ERR;

    if (signum == SIGKILL)
        return SIG_ERR;

    if (handler != SIG_DFL && handler != SIG_IGN && trampoline == 0)
        return SIG_ERR;

    sighandler_t prev = thread->signal_handlers[signum];
    thread->signal_handlers[signum] = handler;

    if (trampoline)
        thread->signal_trampoline = trampoline;

    return prev;
}

int sched_signal_send_thread(sched_thread_t* thread, int signum) {
    if (!thread || !_valid(signum))
        return -1;

    sighandler_t handler = _get_handler(thread, signum);
    if (handler == SIG_IGN)
        return 0;

    _mark_pending(thread, signum);

    if (thread->state == THREAD_SLEEPING) {
        if (thread->blocked_on && thread->blocked_on->list && thread->in_wait_queue) {
            list_remove(thread->blocked_on->list, &thread->wait_node);
            thread->in_wait_queue = false;
            thread->blocked_on = NULL;
        }
        sched_make_runnable(thread);
    }

    return 1;
}

int sched_signal_send_pid(pid_t pid, int signum) {
    sched_thread_t* thread = sched_find_thread(pid);
    if (!thread)
        return -1;

    return sched_signal_send_thread(thread, signum);
}

void sched_signal_deliver_current(arch_int_state_t* state) {
    sched_thread_t* thread = sched_current();
    if (!thread || !thread->user_thread || !state)
        return;

    if (!arch_signal_is_user(state))
        return;

    if (thread->signal_saved_valid)
        return;

    int signum = _next_pending(thread);
    if (!signum)
        return;

    sighandler_t handler = _get_handler(thread, signum);
    if (handler == SIG_IGN) {
        _clear_pending(thread, signum);
        return;
    }

    if (handler == SIG_DFL || thread->signal_trampoline == 0) {
        _clear_pending(thread, signum);
        thread->exit_code = 128 + signum;
        sched_exit();
    }

    thread->signal_saved_state = *state;
    thread->signal_saved_valid = true;
    thread->current_signal = (u32)signum;

    if (!arch_signal_setup_user_stack(thread, state, handler, signum)) {
        thread->signal_saved_valid = false;
        thread->current_signal = 0;
        _clear_pending(thread, signum);
        thread->exit_code = 128 + signum;
        sched_exit();
    }

    _clear_pending(thread, signum);
}

bool sched_signal_sigreturn(sched_thread_t* thread, arch_int_state_t* state) {
    if (!thread || !state || !thread->signal_saved_valid)
        return false;

    *state = thread->signal_saved_state;
    thread->signal_saved_valid = false;
    thread->current_signal = 0;
    thread->context = (uintptr_t)state;
    return true;
}

bool sched_signal_has_pending(sched_thread_t* thread) {
    if (!thread)
        return false;

    return thread->signal_pending != 0;
}
