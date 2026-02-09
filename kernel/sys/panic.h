#pragma once

#include <base/macros.h>
#include <log/log.h>

extern void panic_prepare(void);
extern void panic_halt(void);
void panic_trace(void);

#define panic(...)                              \
    ({                                          \
        panic_prepare();                        \
        log_fatal("Kernel panic: "__VA_ARGS__); \
        panic_trace();                          \
        panic_halt();                           \
    })

#define assert(expression)                              \
    ({                                                  \
        typeof(expression) __as_e = (expression);       \
                                                        \
        if (UNLIKELY(!__as_e))                          \
            panic("Assertion failed: %s", #expression); \
    })
