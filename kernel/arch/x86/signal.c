#include <arch/arch.h>
#include <arch/signal.h>
#include <sched/scheduler.h>
#include <string.h>
#include <x86/asm.h>

static bool _write_user(sched_thread_t* thread, uintptr_t addr, const void* src, size_t len) {
    if (!thread || !thread->vm_space || !src || !len)
        return false;

    uintptr_t base = thread->user_stack_base;
    size_t size = thread->user_stack_size;

    if (!base || !size)
        return false;

    if (addr < base || addr + len > base + size)
        return false;

    unsigned long flags = irq_save();
    sched_preempt_disable();

    arch_vm_switch(thread->vm_space);
    memcpy((void*)addr, src, len);

    sched_preempt_enable();
    irq_restore(flags);

    return true;
}

bool arch_signal_is_user(const arch_int_state_t* state) {
    if (!state)
        return false;

    return (state->s_regs.cs & 0x3) == 3;
}

bool arch_signal_setup_user_stack(
    sched_thread_t* thread,
    arch_int_state_t* state,
    sighandler_t handler,
    int signum
) {
    if (!thread || !state || !handler)
        return false;

#if defined(__x86_64__)
    u64 rsp = state->s_regs.rsp;

    if (rsp < 128 + sizeof(u64))
        return false;

    rsp -= 128;
    rsp -= sizeof(u64);

    u64 ret = (u64)thread->signal_trampoline;

    if (!_write_user(thread, (uintptr_t)rsp, &ret, sizeof(ret)))
        return false;

    state->s_regs.rsp = rsp;
    state->s_regs.rip = (u64)handler;
    state->g_regs.rdi = (u64)signum;
#else
    u32 esp = state->s_regs.esp;
    if (esp < 2 * sizeof(u32))
        return false;

    esp -= 2 * sizeof(u32);
    u32 ret = (u32)thread->signal_trampoline;
    u32 sig = (u32)signum;

    if (!_write_user(thread, (uintptr_t)esp, &ret, sizeof(ret)))
        return false;

    if (!_write_user(thread, (uintptr_t)(esp + sizeof(u32)), &sig, sizeof(sig)))
        return false;

    state->s_regs.esp = esp;
    state->s_regs.eip = (u32)handler;
#endif

    return true;
}
