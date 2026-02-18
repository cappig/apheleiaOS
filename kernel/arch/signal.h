#pragma once

#include <arch/context.h>
#include <signal.h>

struct sched_thread;

bool arch_signal_is_user(const arch_int_state_t *state);
bool arch_signal_setup_user_stack(
    struct sched_thread *thread,
    arch_int_state_t *state,
    sighandler_t handler,
    int signum
);
