#pragma once

#include <drivers/manager.h>

driver_err_t framebuffer_driver_load(void);
driver_err_t framebuffer_driver_unload(void);
bool framebuffer_driver_busy(void);

extern const driver_desc_t framebuffer_driver_desc;
