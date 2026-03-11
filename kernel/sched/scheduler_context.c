#include "scheduler_internal.h"

static bool
sched_context_stack_window_valid(const sched_thread_t *thread, size_t need_bytes) {
    if (!thread || !thread->context || !thread->stack || !thread->stack_size) {
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

bool sched_context_candidate_valid(
    const sched_thread_t *thread,
    const arch_int_state_t *state
) {
    if (!thread || !state || !thread->stack || !thread->stack_size) {
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

    arch_word_t cs = state->s_regs.cs;
    if ((cs & 0x3) != 0x3) {
        return true;
    }

    size_t user_frame_need = sregs_off + sizeof(state->s_regs);
    if ((size_t)(stack_end - ctx) < user_frame_need) {
        return false;
    }

    return true;
}

bool sched_context_valid_ex(
    const sched_thread_t *thread,
    const char **reason_out
) {
#define SCHED_CTX_FAIL(reason_literal)                                            \
    do {                                                                          \
        if (reason_out) {                                                         \
            *reason_out = reason_literal;                                         \
        }                                                                         \
        return false;                                                             \
    } while (0)

    if (!thread || !thread->context) {
        SCHED_CTX_FAIL("missing-thread-or-context");
    }

    const arch_int_state_t *state = (const arch_int_state_t *)thread->context;
    size_t sregs_off = offsetof(arch_int_state_t, s_regs);
    size_t kernel_frame_need = sregs_off + (3U * sizeof(arch_word_t));
    if (!sched_context_stack_window_valid(thread, kernel_frame_need)) {
        SCHED_CTX_FAIL("kernel-frame-outside-stack");
    }
#if defined(__x86_64__)
    u64 cs = state->s_regs.cs;
    bool kernel_cs = cs == (u64)GDT_KERNEL_CODE;
    bool user_cs = cs == (u64)(GDT_USER_CODE | 3);

    if (!kernel_cs && !user_cs) {
        SCHED_CTX_FAIL("invalid-cs");
    }

    if (kernel_cs && state->s_regs.rip < SCHED_KERNEL_VADDR_BASE) {
        SCHED_CTX_FAIL("kernel-rip-outside-kernel");
    }

    if (user_cs) {
        if (!thread->user_thread) {
            SCHED_CTX_FAIL("kernel-thread-with-user-cs");
        }

        size_t user_frame_need = sregs_off + sizeof(state->s_regs);
        if (!sched_context_stack_window_valid(thread, user_frame_need)) {
            SCHED_CTX_FAIL("user-frame-outside-stack");
        }

        u64 rip = state->s_regs.rip;
        u64 rsp = state->s_regs.rsp;
        u64 ss = state->s_regs.ss;

        if (rip == 0 || rsp == 0) {
            SCHED_CTX_FAIL("zero-user-rip-or-rsp");
        }

        if (ss != (u64)(GDT_USER_DATA | 3)) {
            SCHED_CTX_FAIL("invalid-user-ss");
        }

        if ((state->s_regs.rflags & 0x2ULL) == 0) {
            SCHED_CTX_FAIL("invalid-user-rflags");
        }

#if SCHED_STRICT_CONTEXT_CHECK
        u64 user_top = (u64)arch_user_stack_top();
        if (user_top && (rip >= user_top || rsp > user_top)) {
            SCHED_CTX_FAIL("user-rip-rsp-above-user-top");
        }

        if (thread->user_stack_base && thread->user_stack_size) {
            u64 user_base = (u64)thread->user_stack_base;
            u64 user_end = user_base + (u64)thread->user_stack_size;
            if (user_end < user_base) {
                SCHED_CTX_FAIL("invalid-user-stack-range");
            }

            if (rsp < user_base || rsp > user_end) {
                SCHED_CTX_FAIL("rsp-outside-thread-user-stack");
            }
        }
#endif
    }
#else
    u32 cs = state->s_regs.cs;
    bool kernel_cs = cs == (u32)GDT_KERNEL_CODE;
    bool user_cs = cs == (u32)(GDT_USER_CODE | 3);

    if (!kernel_cs && !user_cs) {
        SCHED_CTX_FAIL("invalid-cs");
    }

    if (kernel_cs && state->s_regs.eip < (u32)SCHED_KERNEL_VADDR_BASE) {
        SCHED_CTX_FAIL("kernel-eip-outside-kernel");
    }

    if (user_cs) {
        if (!thread->user_thread) {
            SCHED_CTX_FAIL("kernel-thread-with-user-cs");
        }

        size_t user_frame_need = sregs_off + sizeof(state->s_regs);
        if (!sched_context_stack_window_valid(thread, user_frame_need)) {
            SCHED_CTX_FAIL("user-frame-outside-stack");
        }

        u32 eip = state->s_regs.eip;
        u32 esp = state->s_regs.esp;
        u32 ss = state->s_regs.ss;

        if (eip == 0 || esp == 0) {
            SCHED_CTX_FAIL("zero-user-eip-or-esp");
        }

        if (ss != (u32)(GDT_USER_DATA | 3)) {
            SCHED_CTX_FAIL("invalid-user-ss");
        }

        if ((state->s_regs.eflags & 0x2U) == 0) {
            SCHED_CTX_FAIL("invalid-user-eflags");
        }

#if SCHED_STRICT_CONTEXT_CHECK
        u32 user_top = (u32)arch_user_stack_top();
        if (user_top && (eip >= user_top || esp > user_top)) {
            SCHED_CTX_FAIL("user-eip-esp-above-user-top");
        }

        if (thread->user_stack_base && thread->user_stack_size) {
            u32 user_base = (u32)thread->user_stack_base;
            u32 user_end = (u32)(thread->user_stack_base + thread->user_stack_size);
            if (user_end < user_base) {
                SCHED_CTX_FAIL("invalid-user-stack-range");
            }

            if (esp < user_base || esp > user_end) {
                SCHED_CTX_FAIL("esp-outside-thread-user-stack");
            }
        }
#endif
    }
#endif

    if (reason_out) {
        *reason_out = "ok";
    }
    return true;

#undef SCHED_CTX_FAIL
}

bool sched_context_valid(const sched_thread_t *thread) {
    return sched_context_valid_ex(thread, NULL);
}

void
sched_log_invalid_context_detail(const sched_thread_t *thread, const char *reason) {
    if (!thread) {
        return;
    }

    uintptr_t stack_base = (uintptr_t)thread->stack;
    uintptr_t stack_end = stack_base + thread->stack_size;
    uintptr_t ctx = thread->context;

#if defined(__x86_64__)
    u64 cs = 0;
    u64 ip = 0;
    u64 sp = 0;
    u64 ss = 0;

    if (sched_context_stack_window_valid(thread, offsetof(arch_int_state_t, s_regs) + sizeof(u64))) {
        const arch_int_state_t *state = (const arch_int_state_t *)ctx;
        cs = state->s_regs.cs;
        ip = state->s_regs.rip;
        sp = state->s_regs.rsp;
        ss = state->s_regs.ss;
    }

    log_warn(
        "scheduler invalid-context detail pid=%ld (%s) reason=%s ctx=%#llx "
        "stack=[%#llx,%#llx) cs=%#llx ip=%#llx sp=%#llx ss=%#llx "
        "user_stack=[%#llx,%#llx)",
        (long)thread->pid,
        thread->name,
        reason ? reason : "unknown",
        (unsigned long long)ctx,
        (unsigned long long)stack_base,
        (unsigned long long)stack_end,
        (unsigned long long)cs,
        (unsigned long long)ip,
        (unsigned long long)sp,
        (unsigned long long)ss,
        (unsigned long long)thread->user_stack_base,
        (unsigned long long)(thread->user_stack_base + thread->user_stack_size)
    );
#else
    u32 cs = 0;
    u32 ip = 0;
    u32 sp = 0;
    u32 ss = 0;

    if (sched_context_stack_window_valid(thread, offsetof(arch_int_state_t, s_regs) + sizeof(u32))) {
        const arch_int_state_t *state = (const arch_int_state_t *)ctx;
        cs = state->s_regs.cs;
        ip = state->s_regs.eip;
        sp = state->s_regs.esp;
        ss = state->s_regs.ss;
    }

    log_warn(
        "scheduler invalid-context detail pid=%ld (%s) reason=%s ctx=%#llx "
        "stack=[%#llx,%#llx) cs=%#x ip=%#x sp=%#x ss=%#x "
        "user_stack=[%#llx,%#llx)",
        (long)thread->pid,
        thread->name,
        reason ? reason : "unknown",
        (unsigned long long)ctx,
        (unsigned long long)stack_base,
        (unsigned long long)stack_end,
        cs,
        ip,
        sp,
        ss,
        (unsigned long long)thread->user_stack_base,
        (unsigned long long)(thread->user_stack_base + thread->user_stack_size)
    );
#endif
}
