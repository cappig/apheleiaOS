#pragma once

#include <arch/context.h>
#include <base/types.h>

typedef uintptr_t arch_syscall_t;

// Each arch provides <arch_syscall.h> defining inline register accessors:
//   arch_syscall_num, arch_syscall_arg1..4, arch_syscall_set_ret
#include <arch_syscall.h>
