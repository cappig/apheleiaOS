#include "internal.h"
#include <arch/signal.h>
#include <arch/thread.h>

static bool
ctx_stack_valid(const sched_thread_t *thread, size_t need_bytes) {
    if (!thread || !thread->context || !thread->stack || !thread->stack_size) {
        return false;
    }

    if (!arch_kernel_stack_valid(thread)) {
        return false;
    }

    uintptr_t stack_base = (uintptr_t)thread->stack;
    uintptr_t stack_end = stack_base + thread->stack_size;
    uintptr_t ctx = thread->context;

    if (ctx < stack_base || ctx >= stack_end) {
        return false;
    }

    size_t available = (size_t)(stack_end - ctx);
    return need_bytes <= available;
}

bool ctx_candidate_valid(
    const sched_thread_t *thread,
    const arch_int_state_t *state
) {
    if (!thread || !state || !thread->stack || !thread->stack_size) {
        return false;
    }

    if (!arch_kernel_stack_valid(thread)) {
        return false;
    }

    uintptr_t ctx = (uintptr_t)state;
    uintptr_t stack_base = (uintptr_t)thread->stack;
    uintptr_t stack_end = stack_base + thread->stack_size;

    if (stack_end < stack_base || ctx < stack_base || ctx >= stack_end) {
        return false;
    }

    size_t sregs_off = offsetof(arch_int_state_t, s_regs);
    size_t kernel_frame_need = sregs_off + (3U * sizeof(arch_word_t));
    
    if ((size_t)(stack_end - ctx) < kernel_frame_need) {
        return false;
    }

    if (!arch_signal_is_user(state)) {
        return true;
    }

    size_t user_frame_need = sregs_off + sizeof(state->s_regs);

    if ((size_t)(stack_end - ctx) < user_frame_need) {
        return false;
    }

    return true;
}

bool ctx_valid(const sched_thread_t *thread) {
    if (!thread || !thread->context) {
        return false;
    }

    const arch_int_state_t *state = (const arch_int_state_t *)thread->context;
    size_t sregs_off = offsetof(arch_int_state_t, s_regs);
    size_t kernel_frame_need = sregs_off + (3U * sizeof(arch_word_t));

    if (!ctx_stack_valid(thread, kernel_frame_need)) {
        return false;
    }

    bool is_user = arch_signal_is_user(state);

    if (!arch_state_is_valid(state)) {
        return false;
    }

    if (!is_user && arch_kernel_vaddr_base()) {
        if (arch_state_ip(state) < arch_kernel_vaddr_base()) {
            return false;
        }
    }

    if (is_user) {
        if (!thread->user_thread) {
            return false;
        }

        size_t user_frame_need = sregs_off + sizeof(state->s_regs);
        if (!ctx_stack_valid(thread, user_frame_need)) {
            return false;
        }

        arch_word_t ip = arch_state_ip(state);
        arch_word_t sp = arch_state_sp(state);

        if (ip == 0 || sp == 0) {
            return false;
        }

        arch_word_t user_top = arch_user_stack_top();
        if (user_top && (ip >= user_top || sp > user_top)) {
            return false;
        }

        if (thread->user_stack_base && thread->user_stack_size) {
            arch_word_t user_base = (arch_word_t)thread->user_stack_base;
            arch_word_t user_end = user_base + (arch_word_t)thread->user_stack_size;

            if (user_end < user_base || sp < user_base || sp > user_end) {
                return false;
            }
        }
    }

    return true;
}
