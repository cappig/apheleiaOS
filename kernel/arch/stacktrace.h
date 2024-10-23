#pragma once

#include <base/types.h>
#include <boot/proto.h>

typedef struct {
    u64 rbp;
    u64 rip;
} stack_frame;


void load_symbols(boot_handoff* handoff);

isize resolve_symbol(u64 addr);
const char* resolve_symbol_name(u64 addr);

void dump_stack_trace(void);
