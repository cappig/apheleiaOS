#pragma once

#include <base/attributes.h>

NORETURN static inline void halt(void) {
    for (;;) {
        asm volatile("wfi");
    }

    __builtin_unreachable();
}
