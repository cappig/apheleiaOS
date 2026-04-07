#pragma once

#include <drivers/manager.h>

extern const driver_desc_t riscv_virtio_blk_driver_desc;

driver_err_t riscv_virtio_blk_driver_load(void);
driver_err_t riscv_virtio_blk_driver_unload(void);
bool riscv_virtio_blk_driver_busy(void);
