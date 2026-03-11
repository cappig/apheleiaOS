#pragma once

#include <drivers/manager.h>

driver_err_t serial_driver_load(void);
driver_err_t serial_driver_unload(void);
bool serial_driver_busy(void);

extern const driver_desc_t serial_driver_desc;
