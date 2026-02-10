#pragma once

#include <arch/context.h>
#include <base/types.h>
#include <stdbool.h>

typedef uintptr_t arch_word_t;

struct sched_thread;
uintptr_t arch_build_kernel_stack(struct sched_thread* thread, uintptr_t entry_point);
arch_word_t arch_user_stack_top(void);
bool arch_is_64bit(void);
void arch_state_set_return(arch_int_state_t* state, arch_word_t value);
void arch_state_set_user_entry(arch_int_state_t* state, arch_word_t entry, arch_word_t stack_top);
