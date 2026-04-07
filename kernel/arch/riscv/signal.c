#include <arch/arch.h>
#include <arch/signal.h>
#include <arch/thread.h>
#include <base/macros.h>
#include <sched/scheduler.h>

bool arch_signal_is_user(const arch_int_state_t *state) {
    return arch_state_cs(state) == arch_user_cs();
}

bool arch_signal_setup_user_stack(
    sched_thread_t *thread,
    arch_int_state_t *state,
    sighandler_t handler,
    int signum
) {
    if (!thread || !state || !handler) {
        return false;
    }

    uintptr_t sp = state->s_regs.sp;
    if (!sp) {
        return false;
    }

    uintptr_t aligned = ALIGN_DOWN(sp, 16);
    state->s_regs.sp = aligned;
    state->g_regs.ra = (uintptr_t)thread->signal_trampoline;
    state->g_regs.a0 = (uintptr_t)signum;
    state->s_regs.sepc = (uintptr_t)handler;
    return true;
}
