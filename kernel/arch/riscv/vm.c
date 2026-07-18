#include <arch/arch.h>
#include <arch/paging.h>
#include <base/macros.h>
#include <data/bitmap.h>
#include <riscv/asm.h>
#include <riscv/mm/physical.h>
#include <riscv/vm.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cpu.h>
#include <sys/lock.h>

struct arch_vm_space {
    page_t *root;
    u16 asid;
};

typedef struct {
    struct arch_vm_space kernel;
    page_t *current_root[MAX_CORES];
    u16 current_asid[MAX_CORES];
    bitmap_word_t asids[DIV_ROUND_UP(RISCV_ASID_COUNT, BITMAP_WORD_SIZE)];
    spinlock_t asid_lock;
} vm_state_t;

static vm_state_t vm = { .asid_lock = SPINLOCK_INIT };

static size_t _current_cpu_id(void) {
    cpu_core_t *core = cpu_current();
    return (core && core->id < MAX_CORES) ? core->id : 0;
}

static bool _leaf_pte(page_t entry) {
    return (entry & (PT_READ | PT_WRITE | PT_EXECUTE)) != 0;
}

static u16 _asid_alloc(void) {
    size_t index = 0;
    unsigned long flags = spin_lock_irqsave(&vm.asid_lock);
    bool found = bitmap_find_first_clear(vm.asids, RISCV_ASID_COUNT, &index);

    if (found) {
        bitmap_set(vm.asids, index);
    }

    spin_unlock_irqrestore(&vm.asid_lock, flags);

    // ASID 0 remains a safe flush-on-switch fallback while the pool is busy.
    return found ? (u16)index : 0;
}

static void _asid_free(u16 asid) {
    if (!asid) {
        return;
    }

    sfence_vma_asid(asid);

    unsigned long flags = spin_lock_irqsave(&vm.asid_lock);
    bitmap_clear(vm.asids, asid);
    spin_unlock_irqrestore(&vm.asid_lock, flags);
}

void vm_init_kernel(page_t *root) {
    vm.kernel.root = root;
    vm.kernel.asid = 0;
    bitmap_set(vm.asids, 0);
}

arch_vm_space_t *arch_vm_kernel(void) {
    return &vm.kernel;
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

    space->asid = _asid_alloc();

#if __riscv_xlen == 64
    memcpy(
        &space->root[GET_LVL3_INDEX(RISCV_KERNEL_BASE)],
        &vm.kernel.root[GET_LVL3_INDEX(RISCV_KERNEL_BASE)],
        (512 - GET_LVL3_INDEX(RISCV_KERNEL_BASE)) * sizeof(page_t)
    );
#else
    memcpy(
        &space->root[GET_LVL2_INDEX(RISCV_KERNEL_BASE)],
        &vm.kernel.root[GET_LVL2_INDEX(RISCV_KERNEL_BASE)],
        (1024 - GET_LVL2_INDEX(RISCV_KERNEL_BASE)) * sizeof(page_t)
    );
#endif

    return space;
}

#if __riscv_xlen == 64
static void _free_tables_64(page_t *root) {
    for (size_t i = 0; i < GET_LVL3_INDEX(RISCV_KERNEL_BASE); i++) {
        page_t entry = root[i];

        if (!(entry & PT_PRESENT) || (vm.kernel.root && entry == vm.kernel.root[i])) {
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

        if (!(entry & PT_PRESENT) || (vm.kernel.root && entry == vm.kernel.root[i])) {
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
    if (!space || space == &vm.kernel) {
        return;
    }

#if __riscv_xlen == 64
    _free_tables_64(space->root);
#else
    _free_tables_32(space->root);
#endif

    for (size_t cpu = 0; cpu < MAX_CORES; cpu++) {
        if (vm.current_root[cpu] == space->root && vm.current_asid[cpu] == space->asid) {
            vm.current_root[cpu] = NULL;
            vm.current_asid[cpu] = 0;
        }
    }

    _asid_free(space->asid);
    free_frames(space->root, 1);
    free(space);
}

void arch_vm_switch(arch_vm_space_t *space) {
    if (!space || !space->root) {
        return;
    }

    size_t cpu_id = _current_cpu_id();
    if (vm.current_root[cpu_id] == space->root && vm.current_asid[cpu_id] == space->asid) {
        return;
    }

    bool first_switch = !vm.current_root[cpu_id];
    vm.current_root[cpu_id] = space->root;
    vm.current_asid[cpu_id] = space->asid;
    riscv_write_satp((uintptr_t)space->root, RISCV_PAGING_MODE, space->asid);

    if (first_switch || !space->asid) {
        sfence_vma();
    }
}

void *arch_vm_root(arch_vm_space_t *space) {
    return space ? space->root : NULL;
}
