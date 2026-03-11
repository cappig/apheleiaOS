#pragma once

#include <base/types.h>

u32 pci_bus_read(u8 bus, u8 slot, u8 func, u8 offset, u8 size);
void pci_bus_write(u8 bus, u8 slot, u8 func, u8 offset, u32 value, u8 size);
bool pci_ecam_addr_supported(u64 addr);
