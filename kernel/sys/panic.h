#pragma once

#include <base/macros.h>
#include <log/log.h>
#include <x86/asm.h>

#define panic(...)                              \
    ({                                          \
        panic_unwind();                         \
        log_fatal("Kernel panic: "__VA_ARGS__); \
        halt();                                 \
    })

#define assert(b)                              \
    ({                                         \
        typeof(b) __as_b = (b);                \
                                               \
        if (UNLIKELY(!__as_b))                 \
            panic("Assertion failed: %s", #b); \
    })


void panic_unwind(void);
