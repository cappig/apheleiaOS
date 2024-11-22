#pragma once

#include <base/attributes.h>
#include <base/macros.h>
#include <log/log.h>
#include <x86/asm.h>

#define panic(...)              \
    ({                          \
        log_fatal(__VA_ARGS__); \
        panic_unwind();         \
    })

#define assert(b)                              \
    ({                                         \
        typeof(b) __as_b = (b);                \
                                               \
        if (UNLIKELY(!__as_b))                 \
            panic("assertion failed: %s", #b); \
    })


NORETURN void panic_unwind(void);
