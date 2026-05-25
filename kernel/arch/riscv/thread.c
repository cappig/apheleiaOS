#include <arch/thread.h>
#include <base/macros.h>
#include <riscv/arch_paging.h>
#include <riscv/asm.h>
#include <sched/scheduler.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cpu.h>

arch_word_t arch_user_stack_top(void) {
    return (arch_word_t)0x80000000ULL;
}

bool arch_is_64bit(void) {
#if __riscv_xlen == 64
    return true;
#else
    return false;
#endif
}

void arch_state_set_return(arch_int_state_t *state, arch_word_t value) {
    if (!state) {
        return;
    }

    state->g_regs.a0 = value;
}

void arch_state_set_user_entry(arch_int_state_t *state, arch_word_t entry, arch_word_t stack_top) {
    if (!state) {
        return;
    }

    state->s_regs.sepc = entry;
    state->s_regs.sp = ALIGN_DOWN(stack_top, 16);
    state->s_regs.sstatus = SSTATUS_SPIE;
}

uintptr_t arch_build_kernel_stack(sched_thread_t *thread, uintptr_t entry_point) {
    if (!thread || !thread->stack || !thread->stack_size) {
        return 0;
    }

    uintptr_t sp = (uintptr_t)thread->stack + thread->stack_size;
    sp = ALIGN_DOWN(sp, 16);
    sp -= sizeof(arch_int_state_t);

    arch_int_state_t *state = (arch_int_state_t *)sp;
    memset(state, 0, sizeof(*state));

    state->g_regs.tp = (arch_word_t)(uintptr_t)cpu_current();
    state->s_regs.sepc = entry_point;
    state->s_regs.sstatus = SSTATUS_SPP | SSTATUS_SPIE | SSTATUS_SUM;

    state->s_regs.sp = (arch_word_t)ALIGN_DOWN((uintptr_t)thread->stack + thread->stack_size, 16);

    return sp;
}

bool arch_kernel_stack_alloc(struct sched_thread *thread) {
    if (!thread || !thread->stack_size) {
        return false;
    }

    thread->stack = malloc(thread->stack_size);
    return thread->stack != NULL;
}

void arch_kernel_stack_free(struct sched_thread *thread) {
    if (!thread || !thread->stack) {
        return;
    }

    free(thread->stack);
    thread->stack = NULL;
}

bool arch_kernel_stack_valid(const struct sched_thread *thread) {
    if (!thread || !thread->stack || !thread->stack_size) {
        return false;
    }

    uintptr_t base = (uintptr_t)thread->stack;
    uintptr_t end = base + thread->stack_size;

    return end > base && (size_t)(end - base) >= sizeof(arch_int_state_t);
}

arch_word_t arch_state_ip(const arch_int_state_t *state) {
    return state ? state->s_regs.sepc : 0;
}

arch_word_t arch_state_sp(const arch_int_state_t *state) {
    return state ? state->s_regs.sp : 0;
}

bool arch_state_flags_sane(const arch_int_state_t *state) {
    (void)state;
    return true;
}

arch_word_t arch_kernel_vaddr_base(void) {
    return (arch_word_t)RISCV_KERNEL_BASE;
}
