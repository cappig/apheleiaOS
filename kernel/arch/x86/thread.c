#include <arch/thread.h>
#include <arch/signal.h>
#include <base/macros.h>
#include <sched/scheduler.h>
#include <x86/gdt.h>


arch_word_t arch_user_stack_top(void) {
#if defined(__x86_64__)
    return (arch_word_t)0x0000000080000000ULL;
#elif defined(__i386__)
    return (arch_word_t)0xB0000000U;
#else
    return 0;
#endif
}

bool arch_is_64bit(void) {
#if defined(__x86_64__)
    return true;
#else
    return false;
#endif
}

void arch_state_set_return(arch_int_state_t *state, arch_word_t value) {
    if (!state) {
        return;
    }

#if defined(__x86_64__)
    state->g_regs.rax = value;
#else
    state->g_regs.eax = (u32)value;
#endif
}

void arch_state_set_user_entry(
    arch_int_state_t *state,
    arch_word_t entry,
    arch_word_t stack_top
) {
    if (!state) {
        return;
    }

#if defined(__x86_64__)
    state->s_regs.rip = entry;
    state->s_regs.cs = (arch_word_t)(GDT_USER_CODE | 3);
    state->s_regs.rflags = 0x202;
    state->s_regs.rsp = stack_top;
    state->s_regs.ss = (arch_word_t)(GDT_USER_DATA | 3);
#else
    state->s_regs.eip = (u32)entry;
    state->s_regs.cs = (arch_word_t)(GDT_USER_CODE | 3);
    state->s_regs.eflags = 0x202;
    state->s_regs.esp = (u32)stack_top;
    state->s_regs.ss = (arch_word_t)(GDT_USER_DATA | 3);
    state->seg_regs.ds = (u32)(GDT_USER_DATA | 3);
    state->seg_regs.es = (u32)(GDT_USER_DATA | 3);
    state->seg_regs.fs = (u32)(GDT_USER_DATA | 3);
    state->seg_regs.gs = (u32)(GDT_USER_DATA | 3);
#endif
}

arch_int_state_t *arch_thread_user_context(struct sched_thread *thread) {
    if (!thread || !thread->stack || !thread->stack_size) {
        return NULL;
    }

    uintptr_t base = ALIGN((uintptr_t)thread->stack, 16);
    uintptr_t end = (uintptr_t)thread->stack + thread->stack_size;

    if (end < base || (size_t)(end - base) < sizeof(arch_int_state_t)) {
        return NULL;
    }

    return (arch_int_state_t *)base;
}

uintptr_t
arch_build_kernel_stack(sched_thread_t *thread, uintptr_t entry_point) {
    if (!thread) {
        return 0;
    }

    uintptr_t sp = (uintptr_t)thread->stack + thread->stack_size;
    sp = ALIGN_DOWN(sp, 16);

#if defined(__x86_64__)
    uintptr_t stack_top = sp;
    uintptr_t entry_rsp = stack_top - 24;

    sp -= sizeof(u64);
    *(u64 *)sp = 0;
    sp -= sizeof(u64);
    *(u64 *)sp = GDT_KERNEL_DATA;
    sp -= sizeof(u64);
    *(u64 *)sp = (u64)entry_rsp;
    sp -= sizeof(u64);
    *(u64 *)sp = 0x202; // RFLAGS with IF set
    sp -= sizeof(u64);
    *(u64 *)sp = GDT_KERNEL_CODE;
    sp -= sizeof(u64);
    *(u64 *)sp = (u64)entry_point;

    // Error code and vector
    sp -= sizeof(u64);
    *(u64 *)sp = 0;
    sp -= sizeof(u64);
    *(u64 *)sp = 0;

    // General registers in push order
    u64 regs[15] = {0};

    for (size_t i = 0; i < ARRAY_LEN(regs); i++) {
        sp -= sizeof(u64);
        *(u64 *)sp = regs[i];
    }
#else
    sp -= sizeof(u32);
    *(u32 *)sp = (u32)(uintptr_t)thread->arg;
    sp -= sizeof(u32);
    *(u32 *)sp = (u32)(uintptr_t)thread->entry;
    sp -= sizeof(u32);
    *(u32 *)sp = (u32)(uintptr_t)sched_exit;

    sp -= sizeof(u32);
    *(u32 *)sp = 0x202; // EFLAGS with IF set
    sp -= sizeof(u32);
    *(u32 *)sp = GDT_KERNEL_CODE;
    sp -= sizeof(u32);
    *(u32 *)sp = (u32)entry_point;

    // Error code and vector
    sp -= sizeof(u32);
    *(u32 *)sp = 0;
    sp -= sizeof(u32);
    *(u32 *)sp = 0;

    // Saved segment registers
    sp -= sizeof(u32);
    *(u32 *)sp = (u32)GDT_KERNEL_DATA;
    sp -= sizeof(u32);
    *(u32 *)sp = (u32)GDT_KERNEL_DATA;
    sp -= sizeof(u32);
    *(u32 *)sp = (u32)GDT_KERNEL_DATA;
    sp -= sizeof(u32);
    *(u32 *)sp = (u32)GDT_KERNEL_DATA;

    // General registers in push order
    u32 regs[7] = {0};

    for (size_t i = 0; i < ARRAY_LEN(regs); i++) {
        sp -= sizeof(u32);
        *(u32 *)sp = regs[i];
    }
#endif

    return sp;
}

arch_word_t arch_state_ip(const arch_int_state_t *state) {
#if defined(__x86_64__)
    return (arch_word_t)state->s_regs.rip;
#else
    return (arch_word_t)state->s_regs.eip;
#endif
}

arch_word_t arch_state_sp(const arch_int_state_t *state) {
#if defined(__x86_64__)
    return (arch_word_t)state->s_regs.rsp;
#else
    return (arch_word_t)state->s_regs.esp;
#endif
}

static arch_word_t arch_state_flags(const arch_int_state_t *state) {
#if defined(__x86_64__)
    return (arch_word_t)state->s_regs.rflags;
#else
    return (arch_word_t)state->s_regs.eflags;
#endif
}

bool arch_state_is_valid(const arch_int_state_t *state) {
    if (!state) {
        return false;
    }

    // bit 1 of FLAGS/EFLAGS/RFLAGS is architecturally reserved and must be 1
    if ((arch_state_flags(state) & 0x2) == 0) {
        return false;
    }

    if (arch_signal_is_user(state)) {
        return state->s_regs.cs == (arch_word_t)(GDT_USER_CODE | 3) &&
               state->s_regs.ss == (arch_word_t)(GDT_USER_DATA | 3);
    }

    if (state->s_regs.cs != (arch_word_t)GDT_KERNEL_CODE) {
        return false;
    }

    extern char __kernel_end;
    arch_word_t ip = arch_state_ip(state);
    arch_word_t image_end = (arch_word_t)(uintptr_t)&__kernel_end;

    if (!ip || ip < arch_kernel_vaddr_base()) {
        return false;
    }

    return !image_end || ip < image_end;
}

arch_word_t arch_kernel_vaddr_base(void) {
#if defined(__x86_64__)
    return (arch_word_t)0xffffffff80000000ULL;
#else
    return (arch_word_t)0xc0000000U;
#endif
}
