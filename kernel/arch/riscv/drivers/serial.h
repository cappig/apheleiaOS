#pragma once

#include <drivers/manager.h>

extern const driver_desc_t serial_driver_desc;

driver_err_t serial_driver_load(void);
driver_err_t serial_driver_unload(void);
bool serial_driver_busy(void);
void serial_rx_push(char ch);
