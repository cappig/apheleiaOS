#pragma once

// x86 syscall register accessors
// Requires arch_int_state_t and arch_syscall_t to be defined before inclusion

static inline arch_syscall_t arch_syscall_num(const arch_int_state_t *state) {
#if defined(__x86_64__)
    return (arch_syscall_t)state->g_regs.rax;
#else
    return (arch_syscall_t)state->g_regs.eax;
#endif
}

static inline arch_syscall_t arch_syscall_arg1(const arch_int_state_t *state) {
#if defined(__x86_64__)
    return (arch_syscall_t)state->g_regs.rdi;
#else
    return (arch_syscall_t)state->g_regs.ebx;
#endif
}

static inline arch_syscall_t arch_syscall_arg2(const arch_int_state_t *state) {
#if defined(__x86_64__)
    return (arch_syscall_t)state->g_regs.rsi;
#else
    return (arch_syscall_t)state->g_regs.ecx;
#endif
}

static inline arch_syscall_t arch_syscall_arg3(const arch_int_state_t *state) {
#if defined(__x86_64__)
    return (arch_syscall_t)state->g_regs.rdx;
#else
    return (arch_syscall_t)state->g_regs.edx;
#endif
}

static inline arch_syscall_t arch_syscall_arg4(const arch_int_state_t *state) {
#if defined(__x86_64__)
    return (arch_syscall_t)state->g_regs.r10;
#else
    return (arch_syscall_t)state->g_regs.esi;
#endif
}

static inline void
arch_syscall_set_ret(arch_int_state_t *state, arch_syscall_t value) {
#if defined(__x86_64__)
    state->g_regs.rax = (u64)value;
#else
    state->g_regs.eax = (u32)value;
#endif
}
