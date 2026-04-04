#pragma once

#include <base/types.h>
#include <parse/fdt.h>

bool virtio_init(const fdt_reg_t *regs, size_t reg_count);
bool virtio_read(u64 sector, void *buffer, u32 sector_count);
