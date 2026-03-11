#pragma once

#include <drivers/manager.h>
#include <stdbool.h>

driver_err_t xhci_driver_load(void);
driver_err_t xhci_driver_unload(void);
bool xhci_driver_busy(void);

extern const driver_desc_t xhci_driver_desc;
