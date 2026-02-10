#pragma once

#include <base/types.h>

#if defined(__x86_64__)
#include <x86/paging64.h>
#elif defined(__i386__)
#include <x86/paging32.h>
#else
#error "Unsupported architecture for paging"
#endif

static inline bool arch_supports_nx(void) {
#if defined(__x86_64__)
    return true;
#else
    return false;
#endif
}

static inline u64 arch_user_stack_flags(void) {
    u64 flags = PT_USER | PT_WRITE;
    if (arch_supports_nx())
        flags |= PT_NO_EXECUTE;
    return flags;
}
