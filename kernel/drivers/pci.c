#include "pci.h"

#include <base/addr.h>
#include <base/types.h>
#include <data/list.h>
#include <log/log.h>
#include <string.h>
#include <x86/asm.h>

#include "drivers/acpi.h"
#include "mem/heap.h"
#include "mem/virtual.h"

// A list of PCI devices discovered during boot
static linked_list* devices;


static u64 _ecam_addr(u64 base, u8 bus, u8 slot, u8 func) {
    u64 pddr = base + ((bus << 20) | (slot << 15) | (func << 12));
    u64 vaddr = ID_MAPPED_VADDR(pddr);

    return vaddr;
}

// All PCI devices must support this method
static u32 _read_legacy(u8 bus, u8 slot, u8 func, u8 offset, usize bytes) {
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

static void _probe_slot_legacy(usize bus, usize slot) {
    u8 type = _read_legacy(bus, slot, 0, PCI_REG(header_type), PCI_REG_SIZE(header_type));
    usize funcs = (type & 0x80) ? 8 : 1; // Is this a multifunction device

    for (usize func = 0; func < funcs; func++) {
        u16 vendor = _read_legacy(bus, slot, func, PCI_REG(vendor_id), PCI_REG_SIZE(vendor_id));

        if (vendor == PCI_NONE)
            continue;

        pci_found* device = kcalloc(sizeof(pci_found));
        u8* dev_header = (u8*)&device->header;

        device->base = ~0;
        device->bus = bus;
        device->slot = slot;
        device->func = func;

        // FIXME: This should not be done one byte at a time
        for (usize i = 0; i < sizeof(pci_device); i++)
            dev_header[i] = _read_legacy(bus, slot, func, i, 1);

        list_append(devices, list_create_node(device));
    }
}

static void _probe_slot_express(u64 base, usize bus, usize slot) {
    pci_header* header = (pci_header*)_ecam_addr(base, bus, slot, 0);
    usize funcs = (header->header_type & 0x80) ? 8 : 1;

    for (usize func = 0; func < funcs; func++) {
        pci_header* current = (pci_header*)_ecam_addr(base, bus, slot, func);

        if (current->vendor_id == PCI_NONE)
            continue;

        pci_found* device = kcalloc(sizeof(pci_found));
        u8* dev_header = (u8*)&device->header;

        device->base = base;
        device->bus = bus;
        device->slot = slot;
        device->func = func;

        memcpy(dev_header, current, sizeof(pci_header));

        list_append(devices, list_create_node(device));
    }
}

static void init_legacy() {
    // How many PCI host controllers do we have?
    u8 type = _read_legacy(0, 0, 0, PCI_REG(header_type), PCI_REG_SIZE(header_type));
    usize funcs = (type & 0x80) ? 8 : 1;

    for (usize func = 0; func < funcs; func++) {
        u16 vendor = _read_legacy(0, 0, func, PCI_REG(vendor_id), PCI_REG_SIZE(vendor_id));

        if (vendor == PCI_NONE)
            break;

        for (usize slot = 0; slot < 32; slot++)
            _probe_slot_legacy(func, slot);
    }
}

static void init_express(mcfg* table) {
    usize table_len = table->header.length - sizeof(sdt_header) - sizeof(u64);
    usize len = table_len / sizeof(mcfg_entry);

    for (usize i = 0; i < len; i++) {
        mcfg_entry* entry = &table->entries[i];

        // Since PCIE addresses are 64 bits wide it's possible that the returned address is above
        // the 4GiB bootloader identity mapped region
        u64 region_size = (entry->end_bus - entry->start_bus) << 20;
        identity_map(
            (page_table*)read_cr3(),
            entry->base_addr,
            entry->base_addr + region_size,
            ID_MAP_OFFSET,
            PT_PRESENT | PT_WRITE,
            false
        );

        for (usize j = entry->start_bus; j <= entry->end_bus; j++)
            for (usize slot = 0; slot < 32; slot++)
                _probe_slot_express(entry->base_addr, j, slot);
    }
}

usize pci_init() {
    devices = list_create();

    mcfg* table = (mcfg*)(uptr)acpi_find_table("MCFG");

    if (table)
        init_express(table);
    else
        init_legacy();

    log_info("Detected %zd devices on %s bus", devices->length, table ? "PCIE" : "PCI");

    return devices->length;
}


// TODO: this function only returns the conventional 256 bytes, not the full 4096 bytes that PCIE has
pci_device* pci_find_device(u8 class, u8 subclass, pci_device* from) {
    list_node* start = (from) ? ((list_node*)list_find(devices, from)) : devices->head;

    foreach_from(node, start) {
        pci_found* dev = (pci_found*)node->data;
        pci_header* header = &dev->header;

        if ((header->class == class) && (header->subclass == subclass)) {
            u8* ret = kmalloc(256);

            if (dev->base != (u64)-1) {
                u64 addr = _ecam_addr(dev->base, dev->bus, dev->slot, dev->func);
                memcpy(ret, (u8*)addr, 256);
            } else {
                for (usize i = 0; i < sizeof(pci_device); i++)
                    ret[i] = _read_legacy(dev->bus, dev->slot, dev->func, i, 1);
            }

            return (pci_device*)ret;
        }
    }

    return NULL;
}

void pci_destroy_device(pci_device* dev) {
    kfree(dev);
}


const char* pci_stringify_class(u8 class) {
    if (class <= 0x13)
        return pci_class_strings[class];

    if (class == 0x40)
        return pci_class_strings[21];

    if (class == 0xff)
        return pci_class_strings[22];

    return pci_class_strings[20];
}

void dump_pci_devices() {
    log_debug("Dump of detected PCI devices:");

    foreach (device, devices) {
        pci_found* dev = device->data;

        log_debug(
            "[ class:%#.2x subclass:%.2x prog_if:%.2x ]",
            dev->header.class,
            dev->header.subclass,
            dev->header.prog_if
        );
    }
}
