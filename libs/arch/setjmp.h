#pragma once

#if defined(__x86_64__) || defined(__i386__)
#include <arch/x86/setjmp.h>
#elif defined(__riscv)
#include <arch/riscv/setjmp.h>
#else
#error "Unsupported architecture"
#endif
