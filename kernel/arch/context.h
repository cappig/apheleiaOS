#pragma once

#if defined(__x86_64__) || defined(__i386__)
#include <x86/idt.h>

typedef int_state_t arch_int_state_t;
#else
typedef struct arch_int_state arch_int_state_t;
#endif
