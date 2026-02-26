#pragma once

#include <drivers/manager.h>

driver_err_t ps2_driver_load(void);
driver_err_t ps2_driver_unload(void);
bool ps2_driver_busy(void);

extern const driver_desc_t ps2_driver_desc;
