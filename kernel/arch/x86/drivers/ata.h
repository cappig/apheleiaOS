#pragma once

#include <drivers/manager.h>
#include <stdbool.h>

driver_err_t ata_driver_load(void);
driver_err_t ata_driver_unload(void);
bool ata_driver_busy(void);

extern const driver_desc_t ata_driver_desc;
