#pragma once

#include <base/types.h>
#include <stddef.h>

// Registers the WC range already installed in the x86_64 linear map by the
// bootloader. Other WC requests use the transient physical window.
void x86_phys_set_wc(u64 paddr, size_t size);
