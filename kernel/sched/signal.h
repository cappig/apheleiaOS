#pragma once

#include <aos/signals.h>
#include <base/attributes.h>
#include <base/types.h>

#include "sched/process.h"
#include "x86/regs.h"

#define SIGNAL_MAGIC 0x516beef

// This stricture gets pushed to the processes user stack
typedef struct PACKED {
    u64 ret;

    gen_regs g_regs;

    u32 signal;
    u32 magic;

    u64 rip;
    u64 rflags;
} sig_state;


bool process_signal_defaults(process* proc);

bool signal_set_handler(process* proc, usize signum, sighandler_fn handler);
void signal_send(process* proc, usize signum);

usize signal_get_pending(process* proc);

void prepare_signal(process* proc, usize signum);
bool signal_return(process* proc);
