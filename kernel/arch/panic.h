#pragma once

#include <base/attributes.h>
#include <log/log.h>
#include <x86/asm.h>

#define panic(...)              \
    ({                          \
        log_fatal(__VA_ARGS__); \
        panic_unwind();         \
    })

NORETURN
void panic_unwind();
