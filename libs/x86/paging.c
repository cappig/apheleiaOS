#include "paging.h"

#include "asm.h"


bool supports_1gib_pages() {
    cpuid_regs r = {0};
    cpuid(CPUID_EXTENDED_INFO, &r);

    if (r.edx & CPUID_EI_1G_PAGES)
        return true;

    return false;
}

page_table* page_table_vaddr(page_table* parent) {
    u64 paddr = parent->bits.addr << PAGE_SHIFT;
    page_table* ret = (page_table*)(uptr)ID_MAPPED_VADDR(paddr);

    return ret;
}
