#include "cpu.h"

#include <arch/arch.h>
#include <string.h>
#include <sys/panic.h>

size_t core_count = 1;
size_t core_online_count = 1;
cpu_core_t cores_local[MAX_CORES] = {0};

static cpu_core_t *cpu_boot_local = NULL;

static bool _cpu_ptr_is_core(cpu_core_t *core) {
    uintptr_t ptr = (uintptr_t)core;
    uintptr_t first = (uintptr_t)&cores_local[0];
    uintptr_t last = (uintptr_t)&cores_local[MAX_CORES];

    if (ptr < first || ptr >= last) {
        return false;
    }

    return ((ptr - first) % sizeof(cpu_core_t)) == 0;
}

cpu_core_t *cpu_current(void) {
    cpu_core_t *core = arch_cpu_get_local();

    if (_cpu_ptr_is_core(core)) {
        return core;
    }

    if (_cpu_ptr_is_core(cpu_boot_local)) {
        return cpu_boot_local;
    }

    return &cores_local[0];
}

void cpu_set_current(cpu_core_t *core) {
    cpu_boot_local = core;
    arch_cpu_set_local(core);
}

NORETURN void cpu_halt(void) {
    for (;;) {
        arch_cpu_wait();
    }
}

void cpu_init_core(size_t id) {
    assert(id < MAX_CORES);

    cpu_core_t *core = &cores_local[id];

    memset(core, 0, sizeof(*core));

    core->valid = true;
    core->online = false;
    core->id = id;
}

void cpu_init_boot(void) {
    core_count = 1;
    core_online_count = 0;

    cpu_init_core(0);
    cpu_set_current(&cores_local[0]);
    cpu_set_online(&cores_local[0], true);
}

cpu_core_t *cpu_find_by_lapic(u32 lapic_id) {
    size_t count = core_count;
    if (count > MAX_CORES) {
        count = MAX_CORES;
    }

    for (size_t i = 0; i < count; i++) {
        cpu_core_t *core = &cores_local[i];

        if (!core->valid) {
            continue;
        }

        if (core->lapic_id == lapic_id) {
            return core;
        }
    }

    return NULL;
}

void cpu_set_online(cpu_core_t *core, bool online) {
    if (!core || !core->valid) {
        return;
    }

    bool was_online = core->online;
    if (was_online == online) {
        return;
    }

    core->online = online;

    if (online) {
        __atomic_add_fetch(&core_online_count, 1, __ATOMIC_SEQ_CST);
    } else {
        __atomic_sub_fetch(&core_online_count, 1, __ATOMIC_SEQ_CST);
    }
}
