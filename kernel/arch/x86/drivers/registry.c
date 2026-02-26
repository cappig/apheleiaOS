#include <drivers/manager.h>

#include <drivers/framebuffer.h>
#include <drivers/usb_msc.h>
#include <x86/drivers/ahci.h>
#include <x86/drivers/ata.h>
#include <x86/drivers/ps2.h>
#include <x86/drivers/serial.h>
#include <x86/drivers/usb_xhci.h>
#include <drivers/registry.h>

static const driver_desc_t *const drivers[] = {
    &ps2_driver_desc,
    &ata_driver_desc,
    &ahci_driver_desc,
    &xhci_driver_desc,
    &usb_msc_driver_desc,
    &framebuffer_driver_desc,
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
