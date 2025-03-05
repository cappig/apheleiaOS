#pragma once

#include <base/macros.h>
#include <log/log.h>
#include <x86/asm.h>

#define panic(...)                              \
    ({                                          \
        panic_prepare();                        \
        log_fatal("Kernel panic: "__VA_ARGS__); \
        halt();                                 \
    })

#define assert(expression)                              \
    ({                                                  \
        typeof(expression) __as_e = (expression);       \
                                                        \
        if (UNLIKELY(!__as_e))                          \
            panic("Assertion failed: %s", #expression); \
    })


void panic_prepare(void);
