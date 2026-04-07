#include <arch/arch.h>
#include <arch/paging.h>
#include <base/macros.h>
#include <riscv/asm.h>
#include <riscv/vm.h>
#include <riscv/mm/physical.h>
#include <stdlib.h>
#include <string.h>

struct arch_vm_space {
    page_t *root;
};

static struct arch_vm_space kernel_space = {0};

static bool _leaf_pte(page_t entry) {
    return (entry & (PT_READ | PT_WRITE | PT_EXECUTE)) != 0;
}

void riscv_vm_init_kernel(page_t *root) {
    kernel_space.root = root;
}

arch_vm_space_t *arch_vm_kernel(void) {
    return &kernel_space;
}

arch_vm_space_t *arch_vm_create_user(void) {
    arch_vm_space_t *space = malloc(sizeof(*space));
    if (!space) {
        return NULL;
    }

    space->root = alloc_frames(1);
    if (!space->root) {
        free(space);
        return NULL;
    }

    memset(space->root, 0, PAGE_4KIB);

#if __riscv_xlen == 64
    memcpy(
        &space->root[GET_LVL3_INDEX(RISCV_KERNEL_BASE)],
        &kernel_space.root[GET_LVL3_INDEX(RISCV_KERNEL_BASE)],
        (512 - GET_LVL3_INDEX(RISCV_KERNEL_BASE)) * sizeof(page_t)
    );
#else
    memcpy(
        &space->root[GET_LVL2_INDEX(RISCV_KERNEL_BASE)],
        &kernel_space.root[GET_LVL2_INDEX(RISCV_KERNEL_BASE)],
        (1024 - GET_LVL2_INDEX(RISCV_KERNEL_BASE)) * sizeof(page_t)
    );
#endif

    return space;
}

#if __riscv_xlen == 64
static void _free_tables_64(page_t *root) {
    for (size_t i = 0; i < GET_LVL3_INDEX(RISCV_KERNEL_BASE); i++) {
        page_t entry = root[i];
        if (!(entry & PT_PRESENT) || (kernel_space.root && entry == kernel_space.root[i])) {
            continue;
        }

        if (_leaf_pte(entry)) {
            continue;
        }

        page_t *lvl2 = (page_t *)(uintptr_t)page_get_paddr(&entry);
        for (size_t j = 0; j < 512; j++) {
            page_t lvl2e = lvl2[j];
            if (!(lvl2e & PT_PRESENT) || _leaf_pte(lvl2e)) {
                continue;
            }

            page_t *lvl1 = (page_t *)(uintptr_t)page_get_paddr(&lvl2e);
            free_frames(lvl1, 1);
        }

        free_frames(lvl2, 1);
    }
}
#else
static void _free_tables_32(page_t *root) {
    for (size_t i = 0; i < GET_LVL2_INDEX(RISCV_KERNEL_BASE); i++) {
        page_t entry = root[i];
        if (!(entry & PT_PRESENT) || (kernel_space.root && entry == kernel_space.root[i])) {
            continue;
        }

        if (_leaf_pte(entry)) {
            continue;
        }

        page_t *lvl1 = (page_t *)(uintptr_t)page_get_paddr(&entry);
        free_frames(lvl1, 1);
    }
}
#endif

void arch_vm_destroy(arch_vm_space_t *space) {
    if (!space || space == &kernel_space) {
        return;
    }

#if __riscv_xlen == 64
    _free_tables_64(space->root);
#else
    _free_tables_32(space->root);
#endif

    free_frames(space->root, 1);
    free(space);
}

void arch_vm_switch(arch_vm_space_t *space) {
    if (!space || !space->root) {
        return;
    }

    riscv_write_satp((uintptr_t)space->root, RISCV_PAGING_MODE);
}

void *arch_vm_root(arch_vm_space_t *space) {
    return space ? space->root : NULL;
}
