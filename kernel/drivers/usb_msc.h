#pragma once

#include <base/types.h>
#include <drivers/manager.h>

driver_err_t usb_msc_driver_load(void);
driver_err_t usb_msc_driver_unload(void);
bool usb_msc_driver_busy(void);

extern const driver_desc_t usb_msc_driver_desc;
