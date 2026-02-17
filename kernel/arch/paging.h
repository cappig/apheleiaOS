#pragma once

#include <base/types.h>

// Each arch provides <arch_paging.h> defining:
//   page_t, PT_PRESENT, PT_WRITE, PT_USER, PT_NO_EXECUTE, PT_HUGE,
//   PT_NO_CACHE, PT_WRITE_THROUGH, PT_DIRTY, PT_ACCESSED, PT_GLOBAL,
//   PAGE_4KIB, PAGE_2MIB, page_get_paddr(), page_set_paddr()
#include <arch_paging.h>

bool arch_supports_nx(void);

static inline u64 arch_user_stack_flags(void) {
    u64 flags = PT_USER | PT_WRITE;
    if (arch_supports_nx())
        flags |= PT_NO_EXECUTE;
    return flags;
}
