#pragma once

static inline arch_syscall_t arch_syscall_num(const arch_int_state_t *state) {
    return state ? (arch_syscall_t)state->g_regs.a7 : 0;
}

static inline arch_syscall_t arch_syscall_arg1(const arch_int_state_t *state) {
    return state ? (arch_syscall_t)state->g_regs.a0 : 0;
}

static inline arch_syscall_t arch_syscall_arg2(const arch_int_state_t *state) {
    return state ? (arch_syscall_t)state->g_regs.a1 : 0;
}

static inline arch_syscall_t arch_syscall_arg3(const arch_int_state_t *state) {
    return state ? (arch_syscall_t)state->g_regs.a2 : 0;
}

static inline arch_syscall_t arch_syscall_arg4(const arch_int_state_t *state) {
    return state ? (arch_syscall_t)state->g_regs.a3 : 0;
}

static inline void
arch_syscall_set_ret(arch_int_state_t *state, arch_syscall_t value) {
    if (!state) {
        return;
    }

    state->g_regs.a0 = (uintptr_t)value;
}
