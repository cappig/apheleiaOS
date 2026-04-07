#pragma once

#include <drivers/manager.h>

extern const driver_desc_t riscv_serial_driver_desc;

driver_err_t riscv_serial_driver_load(void);
driver_err_t riscv_serial_driver_unload(void);
bool riscv_serial_driver_busy(void);
void riscv_serial_rx_push(char ch);
