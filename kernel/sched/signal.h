#pragma once

#include <arch/context.h>
#include <signal.h>
#include <sys/types.h>

typedef struct sched_thread sched_thread_t;

void sched_signal_init_thread(sched_thread_t* thread);
void sched_signal_reset_thread(sched_thread_t* thread);

sighandler_t sched_signal_set_handler(
    sched_thread_t* thread,
    int signum,
    sighandler_t handler,
    uintptr_t trampoline
);

int sched_signal_send_thread(sched_thread_t* thread, int signum);
int sched_signal_send_pid(pid_t pid, int signum);

void sched_signal_deliver_current(arch_int_state_t* state);
bool sched_signal_sigreturn(sched_thread_t* thread, arch_int_state_t* state);
bool sched_signal_has_pending(sched_thread_t* thread);
