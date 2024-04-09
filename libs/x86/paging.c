#include "paging.h"

#include "asm.h"


bool supports_1gib_pages() {
    cpuid_regs r = {0};
    cpuid(CPUID_EXTENDED_INFO, &r);

    if (r.edx & CPUID_EI_1G_PAGES)
        return true;

    return false;
}
