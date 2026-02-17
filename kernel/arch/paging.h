#pragma once

#include <base/types.h>

#if defined(__x86_64__)
#include <x86/paging64.h>
#include <x86/asm.h>
#elif defined(__i386__)
#include <x86/paging32.h>
#else
#error "Unsupported architecture for paging"
#endif

static inline bool arch_supports_nx(void) {
#if defined(__x86_64__)
    static bool checked = false;
    static bool has_nx = false;

    if (!checked) {
        cpuid_regs_t regs = {0};
        cpuid(0x80000000, &regs);

        if (regs.eax >= CPUID_EXTENDED_INFO) {
            cpuid(CPUID_EXTENDED_INFO, &regs);

            if (regs.edx & CPUID_EI_NX) {
                u64 efer = read_msr(EFER_MSR);

                if (!(efer & EFER_NX)) {
                    write_msr(EFER_MSR, efer | EFER_NX);
                    efer = read_msr(EFER_MSR);
                }

                has_nx = (efer & EFER_NX) != 0;
            }
        }

        checked = true;
    }

    return has_nx;
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
