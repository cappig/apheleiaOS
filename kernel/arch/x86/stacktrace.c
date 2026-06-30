#include <arch/arch.h>
#include <log/log.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/symbols.h>

#define STACKTRACE_MAX 32

typedef struct stack_frame {
    struct stack_frame *next;
    uintptr_t return_addr;
} stack_frame_t;

static void _dump_stack_from(stack_frame_t *frame) {
    log_info("stack trace");

    for (size_t i = 0; frame && i < STACKTRACE_MAX; i++) {
        uintptr_t return_addr = frame->return_addr;
        symbol_entry_t *sym = resolve_symbol((u64)return_addr);

        if (!sym) {
            log_info("<%#llx> (unknown symbol)", (unsigned long long)return_addr);
        } else {
            u64 offset = (u64)return_addr - sym->addr;
            log_info("<%#llx> %s+%#llx", (unsigned long long)return_addr, sym->name, (unsigned long long)offset);
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
