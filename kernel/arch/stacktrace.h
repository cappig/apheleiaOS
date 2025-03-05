#pragma once

#include <base/types.h>
#include <boot/proto.h>

#define STACKTRACE_MAX 20

typedef struct {
    u64 rbp;
    u64 rip;
} stack_frame;


void dump_stack_trace(u64 rbp);
