#pragma once

#include <base/attributes.h>
#include <base/types.h>

#include "sched/process.h"
#include "sched/scheduler.h"

#define MAX_CORES 64

#define FS_BASE        0xC0000100
#define GS_BASE        0xC0000101
#define KERNEL_GS_BASE 0xC0000102

typedef struct {
    bool valid;
    usize id;

    // How many interrupts are nested at this moment
    // We only perform task switches when this reaches 0
    usize nest_depth;

    u32 lapic_id;

    core_scheduler scheduler;
} cpu_core;


extern cpu_core cores_local[MAX_CORES];
extern usize core_count;

// This means that our per cpu thread structure lives at %gs + 0
static UNUSED __seg_gs cpu_core* cpu = 0;

static inline sched_thread* cpu_current_thread(void) {
    return cpu->scheduler.current;
}

static inline sched_process* cpu_current_proc(void) {
    return cpu->scheduler.current->proc;
}


void cpu_set_gs_base(u64 base);

void cpu_init_core(usize id);
