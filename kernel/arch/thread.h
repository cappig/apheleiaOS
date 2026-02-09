#pragma once

#include <arch/context.h>
#include <base/types.h>

#if defined(__x86_64__) || defined(__i386__)
#include <x86/gdt.h>
#endif

#if defined(__x86_64__)
typedef u64 arch_word_t;
#define ARCH_USER_STACK_TOP ((arch_word_t)0x0000000080000000ULL)

static inline void arch_state_set_return(arch_int_state_t* state, arch_word_t value) {
    state->g_regs.rax = value;
}

static inline void
arch_state_set_user_entry(arch_int_state_t* state, arch_word_t entry, arch_word_t stack_top) {
    state->s_regs.rip = entry;
    state->s_regs.cs = (arch_word_t)(GDT_USER_CODE | 3);
    state->s_regs.rflags = 0x202;
    state->s_regs.rsp = stack_top;
    state->s_regs.ss = (arch_word_t)(GDT_USER_DATA | 3);
}
#elif defined(__i386__)
typedef u32 arch_word_t;
#define ARCH_USER_STACK_TOP ((arch_word_t)0xB0000000U)

static inline void arch_state_set_return(arch_int_state_t* state, arch_word_t value) {
    state->g_regs.eax = value;
}

static inline void
arch_state_set_user_entry(arch_int_state_t* state, arch_word_t entry, arch_word_t stack_top) {
    state->s_regs.eip = entry;
    state->s_regs.cs = (arch_word_t)(GDT_USER_CODE | 3);
    state->s_regs.eflags = 0x202;
    state->s_regs.esp = stack_top;
    state->s_regs.ss = (arch_word_t)(GDT_USER_DATA | 3);
}
#else
typedef uintptr_t arch_word_t;
#define ARCH_USER_STACK_TOP ((arch_word_t)0)

static inline void arch_state_set_return(arch_int_state_t* state, arch_word_t value) {
    (void)state;
    (void)value;
}

static inline void
arch_state_set_user_entry(arch_int_state_t* state, arch_word_t entry, arch_word_t stack_top) {
    (void)state;
    (void)entry;
    (void)stack_top;
}
#endif
