#pragma once

#if defined(__x86_64__) || defined(__i386__)
#include <arch/x86/sys.h>
#elif defined(__riscv)
#include <arch/riscv/sys.h>
#else
#error "Unsupported architecture"
#endif
