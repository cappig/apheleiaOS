#pragma once

#include <arch/context.h>
#include <base/macros.h>
#include <log/log.h>

extern void panic_prepare(void);
extern void panic_halt(void);
void panic_dump_state(const arch_int_state_t *state);

#define panic(...)                              \
    ({                                          \
        panic_prepare();                        \
        log_fatal("Kernel panic: "__VA_ARGS__); \
        panic_dump_state(NULL);                 \
        panic_halt();                           \
    })

#define assert(expression)                              \
    ({                                                  \
        typeof(expression) __as_e = (expression);       \
                                                        \
        if (UNLIKELY(!__as_e))                          \
            panic("Assertion failed: %s", #expression); \
    })
