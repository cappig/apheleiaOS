#include "pci.h"

#include <base/types.h>
#include <data/list.h>
#include <log/log.h>
#include <x86/asm.h>

#include "mem/heap.h"

// A list of PCI devices discovered during boot
static linked_list* devices;


static u32 pci_read(u8 bus, u8 slot, u8 func, u8 offset, usize bytes) {
    u32 addr = 0x80000000 | (bus << 16) | (slot << 11) | (func << 8) | (offset & 0xfc);
    outl(PCI_ADDRESS, addr);

    switch (bytes) {
    case 4:
        return inl(PCI_VALUE);
    case 2:
        return inw(PCI_VALUE + (offset & 2));
    case 1:
        return inb(PCI_VALUE + (offset & 3));
    default:
        return PCI_NONE;
    }
}

static pci_device* pci_get_device(u8 bus, u8 slot, u8 func) {
    u8* header = kcalloc(sizeof(pci_device));

    // FIXME: This should not be done one byte at a time
    for (usize i = 0; i < sizeof(pci_device); i++)
        *(header + i) = pci_read(bus, slot, func, i, 1);

    return (pci_device*)header;
}

static void pci_check_slot(usize bus, usize slot) {
    u8 type = pci_read(bus, slot, 0, PCI_REG(header_type), PCI_REG_SIZE(header_type));
    usize funcs = (type & 0x80) ? 8 : 1; // Is this a multifunction device

    for (usize func = 0; func < funcs; func++) {
        u16 vendor = pci_read(bus, slot, func, PCI_REG(vendor_id), PCI_REG_SIZE(vendor_id));

        if (vendor == PCI_NONE)
            continue;

        pci_device* device = pci_get_device(bus, slot, func);

        list_append(devices, list_create_node(device));
    }
}

static void pci_check_bus(usize bus) {
    for (usize slot = 0; slot < 32; slot++)
        pci_check_slot(bus, slot);
}

usize pci_init() {
    devices = list_create();

    // How many PCI host controllers do we have?
    u8 type = pci_read(0, 0, 0, PCI_REG(header_type), PCI_REG_SIZE(header_type));
    usize funcs = (type & 0x80) ? 8 : 1;

    for (usize func = 0; func < funcs; func++) {
        u16 vendor = pci_read(0, 0, func, PCI_REG(vendor_id), PCI_REG_SIZE(vendor_id));

        if (vendor == PCI_NONE)
            break;

        pci_check_bus(func);
    }

    return devices->length;
}

pci_device* pci_find_device(u8 class, u8 subclass, pci_device* from) {
    list_node* start = (from) ? ((list_node*)list_find(devices, from)) : devices->head;

    foreach_from(node, start) {
        pci_device* device = (pci_device*)node->data;
        pci_header* header = &device->header;

        if ((header->class == class) && (header->subclass == subclass))
            return device;
    }

    return NULL;
}

void dump_pci_devices() {
    log_info("Dump of %zd detected PCI devices:", devices->length);

    foreach (device, devices) {
        pci_device* dev = device->data;

        log_debug(
            "[ class:%.2x subclass:%.2x prog_if:%.2x ]",
            dev->header.class,
            dev->header.subclass,
            dev->header.prog_if
        );
    }
}
