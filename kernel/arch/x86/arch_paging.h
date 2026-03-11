#pragma once

// x86 paging types, constants, and helpers
// Selects between 32-bit PAE and 64-bit long mode paging

#if defined(__x86_64__)
#include <x86/paging64.h>
#elif defined(__i386__)
#include <x86/paging32.h>
#else
#error "Unsupported x86 variant"
#endif
