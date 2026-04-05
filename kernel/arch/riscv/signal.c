#include <arch/arch.h>
#include <arch/signal.h>
#include <arch/thread.h>
#include <base/macros.h>
#include <sched/scheduler.h>
#include <string.h>

static bool _write_user(
    sched_thread_t *thread,
    uintptr_t addr,
    const void *src,
    size_t len
) {
    if (!thread || !thread->vm_space || !src || !len) {
        return false;
    }

    uintptr_t base = thread->user_stack_base;
    size_t size = thread->user_stack_size;

    if (!base || !size) {
        return false;
    }

    if (addr < base || addr + len > base + size) {
        return false;
    }

    unsigned long flags = arch_irq_save();
    sched_preempt_disable();

    arch_vm_switch(thread->vm_space);
    memcpy((void *)addr, src, len);

    sched_preempt_enable();
    arch_irq_restore(flags);
    return true;
}

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
