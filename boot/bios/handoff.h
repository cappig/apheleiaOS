#pragma once

#include <base/types.h>


void jump_to_kernel(u64 entry, u64 handoff_struct, u64 stack);
