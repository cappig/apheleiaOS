#pragma once

// x86 implementation of arch_int_state_t
// This is the interrupt/exception frame pushed by the ISR stubs

#include <x86/idt.h>

typedef int_state_t arch_int_state_t;
