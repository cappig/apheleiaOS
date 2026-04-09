#include <drivers/manager.h>
#include <drivers/registry.h>

#include <riscv/drivers/serial.h>

static const driver_desc_t *const drivers[] = {
    &serial_driver_desc,
};

bool register_drivers(void) {
    for (size_t i = 0; i < (sizeof(drivers) / sizeof(drivers[0])); i++) {
        driver_err_t err = driver_register(drivers[i]);
        if (err != DRIVER_OK) {
            return false;
        }
    }

    return true;
}
