#include <arch/arch.h>
#include <log/log.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/symbols.h>

#define STACKTRACE_MAX 32

typedef struct stack_frame {
    struct stack_frame *next;
    uintptr_t ret;
} stack_frame_t;

static void _dump_stack_from(stack_frame_t *frame) {
    log_info("Stack trace:");

    for (size_t i = 0; frame && i < STACKTRACE_MAX; i++) {
        uintptr_t ret = frame->ret;
        symbol_entry_t *sym = resolve_symbol((u64)ret);

        if (!sym) {
            log_info("<%#llx> (unknown symbol)", (unsigned long long)ret);
        } else {
            u64 offset = (u64)ret - sym->addr;
            log_info(
                "<%#llx> %s+%#llx",
                (unsigned long long)ret,
                sym->name,
                (unsigned long long)offset
            );
        }

        if (frame->next <= frame) {
            break;
        }

        frame = frame->next;
    }
}

void arch_dump_stack_trace(void) {
    stack_frame_t *frame = (stack_frame_t *)__builtin_frame_address(0);

    if (frame) {
        frame = frame->next;
    }

    _dump_stack_from(frame);
}
