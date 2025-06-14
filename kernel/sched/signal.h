#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <x86/regs.h>

#include "sched/process.h"

#define SIGNAL_MAGIC 0x516beef

#define EXIT_SIGNAL_BASE 127

// This stricture gets pushed to the processes user stack
typedef struct PACKED {
    u64 ret;

    gen_regs g_regs;

    u32 signal;
    u32 magic;

    u64 rip;
    u64 rflags;
} sig_state;


usize thread_signal_get_pending(sched_thread* thread);

void thread_signal_switch(sched_thread* thread, usize signum);
bool thread_signal_return(sched_thread* thread);

isize signal_send(sched_process* proc, tid_t tid, usize signum);

bool proc_signal_set_handler(sched_process* proc, usize signum, sighandler_t handler);
