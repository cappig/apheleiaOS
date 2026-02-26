#pragma once

#include <base/attributes.h>
#include <base/types.h>

#define MAX_CORES 64

typedef struct {
    bool valid;
    size_t id;
    size_t nest_depth;
    u32 lapic_id;
} cpu_core_t;

extern cpu_core_t cores_local[MAX_CORES];
extern size_t core_count;

cpu_core_t *cpu_current(void);
void cpu_set_current(cpu_core_t *core);
NORETURN void cpu_halt(void);

void cpu_init_core(size_t id);
void cpu_init_boot(void);
