#include "cpu.h"

#include <string.h>
#include <sys/panic.h>
#include <x86/asm.h>

size_t core_count = 1;
cpu_core_t cores_local[MAX_CORES] = {0};

static cpu_core_t* cpu_local = NULL;

cpu_core_t* cpu_current(void) {
    return cpu_local;
}

void cpu_set_current(cpu_core_t* core) {
    cpu_local = core;

#if defined(__x86_64__)
    if (core)
        set_gs_base((u64)(uintptr_t)core);
#endif
}

void cpu_init_core(size_t id) {
    assert(id < MAX_CORES);

    cpu_core_t* core = &cores_local[id];

    memset(core, 0, sizeof(*core));

    core->valid = true;
    core->id = id;
}

void cpu_init_boot(void) {
    core_count = 1;

    cpu_init_core(0);
    cpu_set_current(&cores_local[0]);
}
