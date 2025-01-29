#include "cpu.h"

#include <x86/asm.h>

cpu_core cores_local[MAX_CORES] = {0};


void cpu_set_gs_base(u64 base) {
    write_msr(KERNEL_GS_BASE, base);
    write_msr(GS_BASE, base);

    swapgs();
}
