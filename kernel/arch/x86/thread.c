#include <arch/thread.h>
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
#endif
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
    *(u64 *)sp = 0; // padding
    sp -= sizeof(u64);
    *(u64 *)sp = GDT_KERNEL_DATA; // ss
    sp -= sizeof(u64);
    *(u64 *)sp = (u64)entry_rsp; // rsp
    sp -= sizeof(u64);
    *(u64 *)sp = 0x202; // RFLAGS with IF set
    sp -= sizeof(u64);
    *(u64 *)sp = GDT_KERNEL_CODE;
    sp -= sizeof(u64);
    *(u64 *)sp = (u64)entry_point;

    // Error code and vector.
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

    // Error code and vector.
    sp -= sizeof(u32);
    *(u32 *)sp = 0;
    sp -= sizeof(u32);
    *(u32 *)sp = 0;

    // General registers in push order
    u32 regs[7] = {0};

    for (size_t i = 0; i < ARRAY_LEN(regs); i++) {
        sp -= sizeof(u32);
        *(u32 *)sp = regs[i];
    }
#endif

    return sp;
}
