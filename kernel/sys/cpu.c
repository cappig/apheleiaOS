#include "cpu.h"

#include <string.h>
#include <x86/asm.h>

usize core_count = 1;
cpu_core cores_local[MAX_CORES] = {0}; // could we make this a vector?


// https://wiki.osdev.org/SWAPGS
void cpu_set_gs_base(u64 base) {
    write_msr(KERNEL_GS_BASE, base);
    write_msr(GS_BASE, base);

    swapgs();
}

void cpu_init_core(usize id) {
    cpu_core* core = &cores_local[id];

    memset(core, 0, sizeof(cpu_core));

    core->valid = true;
    core->nest_depth = 0;
}
