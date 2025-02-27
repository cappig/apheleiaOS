#pragma once

#include <base/types.h>
#include <boot/proto.h>

typedef struct {
    u64 rbp;
    u64 rip;
} stack_frame;


void dump_stack_trace(void);
