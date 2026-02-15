#include <base/macros.h>
#include <base/types.h>
#include <string.h>

#include "physical.h"
#include "virtual.h"
#include "x86/asm.h"

static page_t* _walk_pdpt(page_t* pdpt, size_t index, u64 flags) {
    if (pdpt[index] & PT_PRESENT) {
        pdpt[index] |= flags & (PT_USER | PT_WRITE);
        return (page_t*)(uintptr_t)page_get_paddr(&pdpt[index]);
    }

    page_t* pd = alloc_frames(1);
    memset(pd, 0, PAGE_4KIB);

    page_set_paddr(&pdpt[index], (page_t)(uintptr_t)pd);
    pdpt[index] |= (flags | PT_PRESENT) & FLAGS_MASK;

    return pd;
}

static page_t* _split_huge_pd(page_t* pd, size_t index, u64 flags) {
    page_t pde = pd[index];

    if (!(pde & PT_HUGE))
        return (page_t*)(uintptr_t)page_get_paddr(&pd[index]);

    page_t* pt = alloc_frames(1);
    memset(pt, 0, PAGE_4KIB);

    u64 base = page_get_paddr(&pde);
    u64 entry_flags = (pde & FLAGS_MASK) & ~PT_HUGE;

    for (size_t i = 0; i < 512; i++) {
        page_t* entry = &pt[i];
        page_set_paddr(entry, base + i * PAGE_4KIB);
        *entry |= entry_flags;
    }

    pd[index] = 0;
    page_set_paddr(&pd[index], (page_t)(uintptr_t)pt);
    u64 pd_flags = (pde & FLAGS_MASK) & ~PT_HUGE;
    pd_flags |= flags & FLAGS_MASK;
    pd_flags |= PT_PRESENT | PT_WRITE;
    pd[index] |= pd_flags;

    return pt;
}

static page_t* _walk_pd(page_t* pd, size_t index, u64 flags) {
    if (!(pd[index] & PT_PRESENT)) {
        page_t* pt = alloc_frames(1);
        memset(pt, 0, PAGE_4KIB);

        page_set_paddr(&pd[index], (page_t)(uintptr_t)pt);
        pd[index] |= (flags | PT_PRESENT | PT_WRITE) & FLAGS_MASK;

        return pt;
    }

    if (pd[index] & PT_HUGE)
        return _split_huge_pd(pd, index, flags);

    pd[index] |= flags & (PT_USER | PT_WRITE);
    return (page_t*)(uintptr_t)page_get_paddr(&pd[index]);
}

void map_page(page_t* pdpt, size_t size, u64 vaddr, u64 paddr, u64 flags) {
    if (!pdpt)
        return;

    u32 vaddr32 = (u32)vaddr;

    size_t lvl3_index = GET_LVL3_INDEX(vaddr32);
    size_t lvl2_index = GET_LVL2_INDEX(vaddr32);
    size_t lvl1_index = GET_LVL1_INDEX(vaddr32);

    page_t* pd = _walk_pdpt(pdpt, lvl3_index, flags);

    if (size == PAGE_2MIB) {
        page_t* entry = &pd[lvl2_index];

        paddr = ALIGN_DOWN(paddr, PAGE_2MIB);
        page_set_paddr(entry, paddr);

        flags |= PT_HUGE | PT_PRESENT;
        *entry |= flags & FLAGS_MASK;
        return;
    }

    page_t* pt = _walk_pd(pd, lvl2_index, flags);
    page_t* entry = &pt[lvl1_index];

    page_set_paddr(entry, paddr);
    flags |= PT_PRESENT;
    *entry |= flags & FLAGS_MASK;

    tlb_flush(vaddr32);
}

void unmap_page(page_t* pdpt, u64 vaddr) {
    page_t* page = NULL;

    get_page(pdpt, vaddr, &page);

    if (page) {
        *page = 0;
        tlb_flush((u32)vaddr);
    }
}

void map_region(page_t* pdpt, size_t pages, u64 vaddr, u64 paddr, u64 flags) {
    for (size_t i = 0; i < pages; i++) {
        u64 page_vaddr = vaddr + i * PAGE_4KIB;
        u64 page_paddr = paddr + i * PAGE_4KIB;

        map_page(pdpt, PAGE_4KIB, page_vaddr, page_paddr, flags);
    }
}

void identity_map(page_t* pdpt, u64 from, u64 to, u64 map_offset, u64 flags, bool remap) {
    (void)remap;

    from = ALIGN_DOWN(from, PAGE_4KIB);
    to = ALIGN(to, PAGE_4KIB);

    for (u64 i = from; i < to; i += PAGE_4KIB)
        map_page(pdpt, PAGE_4KIB, i + map_offset, i, flags);
}

size_t get_page(page_t* pdpt, u64 vaddr, page_t** entry) {
    u32 vaddr32 = (u32)vaddr;

    size_t lvl3_index = GET_LVL3_INDEX(vaddr32);
    size_t lvl2_index = GET_LVL2_INDEX(vaddr32);
    size_t lvl1_index = GET_LVL1_INDEX(vaddr32);

    *entry = NULL;

    if (!pdpt)
        return 0;

    if (!(pdpt[lvl3_index] & PT_PRESENT))
        return 0;

    page_t* pd = (page_t*)(uintptr_t)page_get_paddr(&pdpt[lvl3_index]);
    page_t pde = pd[lvl2_index];

    if (!(pde & PT_PRESENT))
        return 0;

    if (pde & PT_HUGE && pde & PT_PRESENT) {
        *entry = &pd[lvl2_index];
        return PAGE_2MIB;
    }

    page_t* pt = (page_t*)(uintptr_t)page_get_paddr(&pde);
    page_t* pte = &pt[lvl1_index];

    if (*pte & PT_PRESENT) {
        *entry = pte;
        return PAGE_4KIB;
    }

    return 0;
}
