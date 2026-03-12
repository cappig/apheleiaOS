#include "signal.h"

#include <arch/signal.h>
#include <sched/scheduler.h>
#include <string.h>

static const sighandler_t default_actions[NSIG] = {
    [SIGCHLD] = SIG_IGN,
    [SIGWINCH] = SIG_IGN,
};

static bool signal_valid(int signum) {
    return signum > 0 && signum < NSIG;
}

static bool signal_is_stop(int signum) {
    return (
        signum == SIGTSTP ||
        signum == SIGSTOP ||
        signum == SIGTTIN ||
        signum == SIGTTOU
    );
}

static bool signal_is_continue(int signum) {
    return signum == SIGCONT;
}

static sighandler_t signal_get_handler(sched_thread_t *thread, int signum) {
    if (!thread || !signal_valid(signum)) {
        return SIG_DFL;
    }

    if (signum == SIGKILL || signum == SIGSTOP) {
        return SIG_DFL;
    }

    sighandler_t handler = thread->signal_handlers[signum];

    if (handler == SIG_DFL) {
        sighandler_t def = default_actions[signum];
        return def ? def : SIG_DFL;
    }

    return handler;
}

static void signal_mark_pending(sched_thread_t *thread, int signum) {
    if (!thread || !signal_valid(signum)) {
        return;
    }

    u32 mask = 1u << (signum - 1);
    __atomic_fetch_or(&thread->signal_pending, mask, __ATOMIC_ACQ_REL);
}

static void signal_clear_pending(sched_thread_t *thread, int signum) {
    if (!thread || !signal_valid(signum)) {
        return;
    }

    u32 mask = 1u << (signum - 1);
    __atomic_fetch_and(&thread->signal_pending, ~mask, __ATOMIC_ACQ_REL);
}

static int signal_next_pending(sched_thread_t *thread) {
    if (!thread) {
        return 0;
    }

    u32 pending =
        __atomic_load_n(&thread->signal_pending, __ATOMIC_ACQUIRE) &
        ~__atomic_load_n(&thread->signal_mask, __ATOMIC_ACQUIRE);

    if (!pending) {
        return 0;
    }

    for (int signum = 1; signum < NSIG; signum++) {
        if (pending & (1u << (signum - 1))) {
            return signum;
        }
    }

    return 0;
}

void sched_signal_init_thread(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    memset(thread->signal_handlers, 0, sizeof(thread->signal_handlers));

    thread->signal_pending = 0;
    thread->signal_mask = 0;
    __atomic_store_n(&thread->current_signal, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&thread->signal_saved_valid, 0, __ATOMIC_RELEASE);
    thread->signal_trampoline = 0;
}

void sched_signal_reset_thread(sched_thread_t *thread) {
    sched_signal_init_thread(thread);
}

sighandler_t sched_signal_set_handler(
    sched_thread_t *thread,
    int signum,
    sighandler_t handler,
    uintptr_t trampoline
) {
    if (!thread || !signal_valid(signum)) {
        return SIG_ERR;
    }

    if (signum == SIGKILL || signum == SIGSTOP) {
        return SIG_ERR;
    }

    if (handler != SIG_DFL && handler != SIG_IGN && !trampoline) {
        return SIG_ERR;
    }

    sighandler_t prev = thread->signal_handlers[signum];
    thread->signal_handlers[signum] = handler;

    if (!trampoline) {
        return prev;
    }

    thread->signal_trampoline = trampoline;

    return prev;
}

int sched_signal_send_thread(sched_thread_t *thread, int signum) {
    if (!thread || !signal_valid(signum)) {
        return -1;
    }

    sighandler_t handler = signal_get_handler(thread, signum);
    if (handler == SIG_IGN) {
        return 0;
    }

    // SIGCONT always continues a stopped process, regardless of handler
    if (signal_is_continue(signum)) {
        sched_continue_thread(thread);
    }

    if (signal_is_continue(signum) && handler == SIG_DFL) {
        return 1;
    }

    if (signal_is_stop(signum) && handler == SIG_DFL) {
        sched_stop_thread(thread, signum);
        return 1;
    }

    signal_mark_pending(thread, signum);
    sched_unblock_thread(thread);

    return 1;
}

int sched_signal_send_pid(pid_t pid, int signum) {
    sched_thread_t *thread = sched_find_thread(pid);
    if (!thread) {
        return -1;
    }

    int rc = sched_signal_send_thread(thread, signum);
    thread_put(thread);

    return rc;
}

void sched_signal_deliver_current(arch_int_state_t *state) {
    sched_thread_t *thread = sched_current();
    if (!thread || !thread->user_thread || !state) {
        return;
    }

    if (!arch_signal_is_user(state)) {
        return;
    }

    if (__atomic_load_n(&thread->signal_saved_valid, __ATOMIC_ACQUIRE)) {
        return;
    }

    int signum = signal_next_pending(thread);
    if (!signum) {
        return;
    }

    sighandler_t handler = signal_get_handler(thread, signum);

    if (handler == SIG_IGN) {
        signal_clear_pending(thread, signum);
        return;
    }

    if (handler == SIG_DFL && signal_is_stop(signum)) {
        signal_clear_pending(thread, signum);
        sched_stop_thread(thread, signum);
        return;
    }

    if (handler == SIG_DFL && signal_is_continue(signum)) {
        signal_clear_pending(thread, signum);
        sched_continue_thread(thread);
        return;
    }

    if (handler == SIG_DFL || !thread->signal_trampoline) {
        signal_clear_pending(thread, signum);
        thread->exit_code = 128 + signum;
        sched_exit();
    }

    thread->signal_saved_state = *state;
    __atomic_store_n(&thread->current_signal, (u32)signum, __ATOMIC_RELEASE);
    __atomic_store_n(&thread->signal_saved_valid, 1, __ATOMIC_RELEASE);

    if (!arch_signal_setup_user_stack(thread, state, handler, signum)) {
        __atomic_store_n(&thread->signal_saved_valid, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&thread->current_signal, 0, __ATOMIC_RELEASE);

        signal_clear_pending(thread, signum);

        thread->exit_code = 128 + signum;
        sched_exit();
    }

    signal_clear_pending(thread, signum);
}

bool sched_signal_sigreturn(sched_thread_t *thread, arch_int_state_t *state) {
    if (
        !thread || !state ||
        !__atomic_load_n(&thread->signal_saved_valid, __ATOMIC_ACQUIRE)
    ) {
        return false;
    }

    *state = thread->signal_saved_state;
    __atomic_store_n(&thread->signal_saved_valid, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&thread->current_signal, 0, __ATOMIC_RELEASE);
    thread->context = (uintptr_t)state;

    return true;
}

bool sched_signal_has_pending(sched_thread_t *thread) {
    if (!thread) {
        return false;
    }

    return (
        __atomic_load_n(&thread->signal_pending, __ATOMIC_ACQUIRE) &
        ~__atomic_load_n(&thread->signal_mask, __ATOMIC_ACQUIRE)
    ) != 0;
}
