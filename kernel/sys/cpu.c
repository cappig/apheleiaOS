#include "cpu.h"

#include <arch/arch.h>
#include <string.h>
#include <sys/panic.h>

size_t core_count = 1;
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

    return cpu_boot_local;
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
    core->id = id;
}

void cpu_init_boot(void) {
    core_count = 1;

    cpu_init_core(0);
    cpu_set_current(&cores_local[0]);
}
