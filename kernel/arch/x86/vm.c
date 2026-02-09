#include <arch/arch.h>
#include <base/macros.h>
#include <stdlib.h>
#include <string.h>
#include <x86/asm.h>
#include <x86/boot.h>
#include <x86/mm/physical.h>
#include <x86/mm/virtual.h>

#if defined(__x86_64__)
#include <x86/paging64.h>
#else
#include <x86/paging32.h>
#endif

struct arch_vm_space {
    page_t* root;
};

static struct arch_vm_space kernel_space = {0};

#if defined(__x86_64__)
static void* _phys_map(page_t* paddr) {
    return arch_phys_map((u64)(uintptr_t)paddr, PAGE_4KIB);
}

static void _phys_unmap(void* vaddr) {
    arch_phys_unmap(vaddr, PAGE_4KIB);
}
#endif

arch_vm_space_t* arch_vm_kernel(void) {
    if (!kernel_space.root) {
#if defined(__x86_64__)
        kernel_space.root = (page_t*)(uintptr_t)(read_cr3() & ~0xfffULL);
#else
        kernel_space.root = (page_t*)(uintptr_t)(read_cr3() & ~0x1fULL);
#endif
    }

    return &kernel_space;
}

#if defined(__x86_64__)
static page_t* _clone_kernel_root_64(page_t* kernel_root) {
    page_t* root = alloc_frames(1);

    void* root_map = _phys_map(root);
    if (!root_map)
        return NULL;

    memset(root_map, 0, PAGE_4KIB);

    void* kernel_map = _phys_map(kernel_root);
    if (!kernel_map) {
        _phys_unmap(root_map);
        return NULL;
    }

    memcpy((page_t*)root_map + 256, (page_t*)kernel_map + 256, 256 * sizeof(page_t));

    _phys_unmap(kernel_map);
    _phys_unmap(root_map);

    return root;
}
#else
static void _clone_kernel_pt_32(page_t* dest_pd, page_t* src_pd) {
    for (size_t i = 0; i < 512; i++) {
        page_t pde = src_pd[i];

        if (!(pde & PT_PRESENT)) {
            dest_pd[i] = 0;
            continue;
        }

        if (pde & PT_HUGE) {
            dest_pd[i] = pde;
            continue;
        }

        page_t* src_pt = (page_t*)(uintptr_t)page_get_paddr(&pde);
        page_t* dest_pt = alloc_frames(1);

        memcpy(dest_pt, src_pt, PAGE_4KIB);

        dest_pd[i] = 0;
        page_set_paddr(&dest_pd[i], (page_t)(uintptr_t)dest_pt);
        dest_pd[i] |= (pde & FLAGS_MASK) & ~PT_HUGE;
    }
}

static page_t* _clone_kernel_root_32(page_t* kernel_root) {
    page_t* root = alloc_frames(1);
    memset(root, 0, PAGE_4KIB);

    for (size_t i = 0; i < 4; i++) {
        page_t entry = kernel_root[i];

        if (!(entry & PT_PRESENT)) {
            root[i] = 0;
            continue;
        }

        page_t* src_pd = (page_t*)(uintptr_t)page_get_paddr(&entry);
        page_t* dest_pd = alloc_frames(1);

        _clone_kernel_pt_32(dest_pd, src_pd);

        root[i] = 0;
        page_set_paddr(&root[i], (page_t)(uintptr_t)dest_pd);
        root[i] |= entry & FLAGS_MASK;
    }

    return root;
}
#endif

arch_vm_space_t* arch_vm_create_user(void) {
    arch_vm_space_t* space = malloc(sizeof(*space));
    if (!space)
        return NULL;

    page_t* kernel_root = arch_vm_kernel()->root;

#if defined(__x86_64__)
    space->root = _clone_kernel_root_64(kernel_root);
#else
    space->root = _clone_kernel_root_32(kernel_root);
#endif

    if (!space->root) {
        free(space);
        return NULL;
    }

    return space;
}

#if defined(__x86_64__)
static void _free_tables_64(page_t* root) {
    page_t* root_map = (page_t*)((uintptr_t)root + LINEAR_MAP_OFFSET_64);

    for (size_t i = 0; i < 256; i++) {
        if (!(root_map[i] & PT_PRESENT))
            continue;

        page_t* lvl3 = (page_t*)page_get_paddr(&root_map[i]);
        page_t* lvl3_map = (page_t*)((uintptr_t)lvl3 + LINEAR_MAP_OFFSET_64);

        for (size_t j = 0; j < 512; j++) {
            if (!(lvl3_map[j] & PT_PRESENT))
                continue;

            if (lvl3_map[j] & PT_HUGE)
                continue;

            page_t* lvl2 = (page_t*)page_get_paddr(&lvl3_map[j]);
            page_t* lvl2_map = (page_t*)((uintptr_t)lvl2 + LINEAR_MAP_OFFSET_64);

            for (size_t k = 0; k < 512; k++) {
                if (!(lvl2_map[k] & PT_PRESENT))
                    continue;

                if (lvl2_map[k] & PT_HUGE)
                    continue;

                page_t* lvl1 = (page_t*)page_get_paddr(&lvl2_map[k]);
                free_frames(lvl1, 1);
            }

            free_frames(lvl2, 1);
        }

        free_frames(lvl3, 1);
    }
}
#else
static void _free_tables_32(page_t* root) {
    for (size_t i = 0; i < 4; i++) {
        if (!(root[i] & PT_PRESENT))
            continue;

        page_t* pd = (page_t*)(uintptr_t)page_get_paddr(&root[i]);

        for (size_t j = 0; j < 512; j++) {
            page_t pde = pd[j];

            if (!(pde & PT_PRESENT))
                continue;

            if (pde & PT_HUGE)
                continue;

            page_t* pt = (page_t*)(uintptr_t)page_get_paddr(&pde);
            free_frames(pt, 1);
        }

        free_frames(pd, 1);
    }
}
#endif

void arch_vm_destroy(arch_vm_space_t* space) {
    if (!space || space == &kernel_space)
        return;

#if defined(__x86_64__)
    _free_tables_64(space->root);
#else
    _free_tables_32(space->root);
#endif

    free_frames(space->root, 1);
    free(space);
}

void arch_vm_switch(arch_vm_space_t* space) {
    if (!space || !space->root)
        return;

#if defined(__x86_64__)
    write_cr3((u64)(uintptr_t)space->root);
#else
    write_cr3((u32)(uintptr_t)space->root);
#endif
}

void* arch_vm_root(arch_vm_space_t* space) {
    if (!space)
        return NULL;

    return space->root;
}
