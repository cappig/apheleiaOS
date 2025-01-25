#pragma once

#include <base/types.h>

#include "sched/scheduler.h"

// https://wiki.osdev.org/SWAPGS

#define MAX_CORES 64

#define FS_BASE        0xC0000100
#define GS_BASE        0xC0000101
#define KERNEL_GS_BASE 0xC0000102

typedef struct {
    usize id;

    u32 lapic_id;

    bool sched_running;
    scheduler* sched;
} cpu_core;


extern cpu_core cores_local[MAX_CORES];

// This means that our per cpu thread structure lives at %gs + 0
static __seg_gs cpu_core* cpu = 0;


void cpu_set_gs_base(u64 base);
