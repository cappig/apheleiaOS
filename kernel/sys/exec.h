#pragma once

#include <arch/context.h>
#include <sched/scheduler.h>

sched_thread_t* user_spawn(const char* path);
int user_exec(sched_thread_t* thread, const char* path, arch_int_state_t* state);
