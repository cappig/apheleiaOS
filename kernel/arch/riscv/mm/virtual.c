#include "virtual.h"

#include <base/macros.h>
#include <stdlib.h>
#include <string.h>

#include "physical.h"
#include <riscv/asm.h>

static bool _leaf_pte(page_t entry) {
    return (entry & (PT_READ | PT_WRITE | PT_EXECUTE)) != 0;
}

static page_t _map_flags(u64 flags) {
    page_t pte = PT_PRESENT | PT_ACCESSED | PT_DIRTY | PT_READ;

    if (flags & PT_WRITE) {
        pte |= PT_WRITE;
    }

    if (!(flags & PT_NO_EXECUTE)) {
        pte |= PT_EXECUTE;
    }

    if (flags & PT_USER) {
        pte |= PT_USER;
    }

    if (flags & PT_GLOBAL) {
        pte |= PT_GLOBAL;
    }

    return pte;
}

#if __riscv_xlen == 64
static page_t *_walk_table_once(page_t *table, size_t index) {
    if (table[index] & PT_PRESENT) {
        return (page_t *)(uintptr_t)page_get_paddr(&table[index]);
    }

    page_t *next = alloc_frames(1);
    memset(next, 0, PAGE_4KIB);

    table[index] = 0;
    page_set_paddr(&table[index], (u64)(uintptr_t)next);
    table[index] |= PT_PRESENT;
    return next;
}
#else
static page_t *_walk_table_once(page_t *table, size_t index) {
    if (table[index] & PT_PRESENT) {
        return (page_t *)(uintptr_t)page_get_paddr(&table[index]);
    }

    page_t *next = alloc_frames(1);
    memset(next, 0, PAGE_4KIB);

    table[index] = 0;
    page_set_paddr(&table[index], (u64)(uintptr_t)next);
    table[index] |= PT_PRESENT;
    return next;
}
#endif

void map_page(page_t *root, size_t size, u64 vaddr, u64 paddr, u64 flags) {
    (void)size;

    if (!root) {
        return;
    }

#if __riscv_xlen == 64
    page_t *lvl2 = _walk_table_once(root, GET_LVL3_INDEX(vaddr));
    page_t *lvl1 = _walk_table_once(lvl2, GET_LVL2_INDEX(vaddr));
    page_t *entry = &lvl1[GET_LVL1_INDEX(vaddr)];
#else
    page_t *lvl1 = _walk_table_once(root, GET_LVL2_INDEX(vaddr));
    page_t *entry = &lvl1[GET_LVL1_INDEX(vaddr)];
#endif

    *entry = 0;
    page_set_paddr(entry, paddr);
    *entry |= _map_flags(flags);
    sfence_vma();
}

void unmap_page(page_t *root, u64 vaddr) {
    page_t *entry = NULL;
    (void)get_page(root, vaddr, &entry);

    if (!entry) {
        return;
    }

    *entry = 0;
    sfence_vma();
}

void map_region(page_t *root, size_t pages, u64 vaddr, u64 paddr, u64 flags) {
    for (size_t i = 0; i < pages; i++) {
        map_page(
            root,
            PAGE_4KIB,
            vaddr + i * PAGE_4KIB,
            paddr + i * PAGE_4KIB,
            flags
        );
    }
}

size_t get_page(page_t *root, u64 vaddr, page_t **entry) {
    if (entry) {
        *entry = NULL;
    }

    if (!root) {
        return 0;
    }

#if __riscv_xlen == 64
    page_t lvl2e = root[GET_LVL3_INDEX(vaddr)];
    if (!(lvl2e & PT_PRESENT)) {
        return 0;
    }
    if (_leaf_pte(lvl2e)) {
        if (entry) {
            *entry = &root[GET_LVL3_INDEX(vaddr)];
        }
        return 1ULL << 30;
    }

    page_t *lvl2 = (page_t *)(uintptr_t)page_get_paddr(&lvl2e);
    page_t lvl1e = lvl2[GET_LVL2_INDEX(vaddr)];
    if (!(lvl1e & PT_PRESENT)) {
        return 0;
    }
    if (_leaf_pte(lvl1e)) {
        if (entry) {
            *entry = &lvl2[GET_LVL2_INDEX(vaddr)];
        }
        return PAGE_2MIB;
    }

    page_t *lvl1 = (page_t *)(uintptr_t)page_get_paddr(&lvl1e);
    page_t *pte = &lvl1[GET_LVL1_INDEX(vaddr)];
#else
    page_t lvl1e = root[GET_LVL2_INDEX(vaddr)];
    if (!(lvl1e & PT_PRESENT)) {
        return 0;
    }
    if (_leaf_pte(lvl1e)) {
        if (entry) {
            *entry = &root[GET_LVL2_INDEX(vaddr)];
        }
        return 1ULL << 22;
    }

    page_t *lvl1 = (page_t *)(uintptr_t)page_get_paddr(&lvl1e);
    page_t *pte = &lvl1[GET_LVL1_INDEX(vaddr)];
#endif

    if (!(*pte & PT_PRESENT)) {
        return 0;
    }

    if (entry) {
        *entry = pte;
    }

    return PAGE_4KIB;
}
