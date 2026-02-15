#include "pci.h"

#include <arch/arch.h>
#include <arch/pci.h>
#include <data/list.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>

#include "acpi.h"

static bool pci_is_express = false;
static linked_list_t* pci_devices = NULL;

static u64 _ecam_addr(u64 base, u8 bus, u8 slot, u8 func) {
    return base + ((u64)bus << 20) + ((u64)slot << 15) + ((u64)func << 12);
}

static u32 _read_legacy(u8 bus, u8 slot, u8 func, u8 offset, size_t bytes) {
    return pci_bus_read(bus, slot, func, offset, bytes);
}

static bool _ecam_read_header(u64 base, u8 bus, u8 slot, u8 func, pci_header_t* out) {
    if (!out)
        return false;

    u64 phys = _ecam_addr(base, bus, slot, func);
    void* map = arch_phys_map(phys, sizeof(pci_header_t));

    if (!map)
        return false;

    memcpy(out, map, sizeof(pci_header_t));
    arch_phys_unmap(map, sizeof(pci_header_t));

    return true;
}

static void _probe_slot_legacy(u8 bus, u8 slot) {
    u8 type = (u8)_read_legacy(bus, slot, 0, offsetof(pci_header_t, header_type), 1);
    size_t funcs = (type & 0x80) ? 8 : 1;

    for (size_t func = 0; func < funcs; func++) {
        u16 vendor = (u16)_read_legacy(bus, slot, func, offsetof(pci_header_t, vendor_id), 2);

        if (vendor == PCI_NONE)
            continue;

        pci_found_t* device = calloc(1, sizeof(pci_found_t));
        if (!device)
            return;

        device->base = ~0ULL;
        device->bus = bus;
        device->slot = slot;
        device->func = (u8)func;

        u8* header_bytes = (u8*)&device->header;

        for (size_t i = 0; i < sizeof(pci_header_t); i++)
            header_bytes[i] = (u8)_read_legacy(bus, slot, func, (u8)i, 1);

        list_append(pci_devices, list_create_node(device));
    }
}

static void _probe_slot_express(u64 base, u8 bus, u8 slot) {
    pci_header_t header = {0};

    if (!_ecam_read_header(base, bus, slot, 0, &header))
        return;

    if (header.vendor_id == PCI_NONE)
        return;

    size_t funcs = (header.header_type & 0x80) ? 8 : 1;

    for (size_t func = 0; func < funcs; func++) {
        pci_header_t current = {0};

        if (!_ecam_read_header(base, bus, slot, (u8)func, &current))
            continue;

        if (current.vendor_id == PCI_NONE)
            continue;

        pci_found_t* device = calloc(1, sizeof(pci_found_t));
        if (!device)
            return;

        device->base = base;
        device->bus = bus;
        device->slot = slot;
        device->func = (u8)func;
        device->header = current;

        list_append(pci_devices, list_create_node(device));
    }
}

static void _init_legacy(void) {
    u8 type = (u8)_read_legacy(0, 0, 0, offsetof(pci_header_t, header_type), 1);
    size_t funcs = (type & 0x80) ? 8 : 1;

    for (size_t func = 0; func < funcs; func++) {
        u16 vendor = (u16)_read_legacy(0, 0, (u8)func, offsetof(pci_header_t, vendor_id), 2);

        if (vendor == PCI_NONE)
            break;

        for (u8 slot = 0; slot < 32; slot++)
            _probe_slot_legacy((u8)func, slot);
    }
}

static void _init_express(mcfg_t* table) {
    pci_is_express = true;

    size_t table_len = table->header.length - sizeof(sdt_header_t) - sizeof(u64);
    size_t count = table_len / sizeof(mcfg_entry_t);

    for (size_t i = 0; i < count; i++) {
        mcfg_entry_t* entry = &table->entries[i];

        if (!entry->base_addr)
            continue;

        if (!pci_ecam_addr_supported(entry->base_addr)) {
            log_warn("pci: skipping MCFG entry above 4GiB");
            continue;
        }

        for (u16 bus = entry->start_bus; bus <= entry->end_bus; bus++) {
            for (u8 slot = 0; slot < 32; slot++)
                _probe_slot_express(entry->base_addr, (u8)bus, slot);
        }
    }
}

size_t pci_init(void) {
    pci_devices = list_create();
    if (!pci_devices)
        return 0;

    mcfg_t* table = (mcfg_t*)acpi_find_table("MCFG");

    if (table)
        _init_express(table);
    else
        _init_legacy();

    log_info(
        "pci: detected %zu devices on the %s bus",
        pci_devices->length,
        pci_is_express ? "PCIE" : "PCI"
    );

    return pci_devices->length;
}

// NOTE: returns the conventional 256 bytes, not the full 4096 bytes for PCIe.
pci_device_t* pci_find_device(u8 class, u8 subclass, pci_device_t* from) {
    list_node_t* start = pci_devices ? pci_devices->head : NULL;
    bool matched_from = (from == NULL);

    ll_foreach_from(node, start) {
        pci_found_t* dev = node->data;
        pci_header_t* header = &dev->header;

        if (!matched_from) {
            if (header->vendor_id == from->header.vendor_id &&
                header->device_id == from->header.device_id &&
                header->class == from->header.class && header->subclass == from->header.subclass &&
                header->prog_if == from->header.prog_if) {
                matched_from = true;
            }

            continue;
        }

        if (header->class == class && header->subclass == subclass) {
            u8* ret = malloc(256);
            if (!ret)
                return NULL;

            if (dev->base != (u64)-1) {
                u64 phys = _ecam_addr(dev->base, dev->bus, dev->slot, dev->func);
                void* map = arch_phys_map(phys, 256);

                if (!map) {
                    free(ret);
                    return NULL;
                }

                memcpy(ret, map, 256);
                arch_phys_unmap(map, 256);
            } else {
                for (size_t i = 0; i < 256; i++)
                    ret[i] = (u8)_read_legacy(dev->bus, dev->slot, dev->func, (u8)i, 1);
            }

            return (pci_device_t*)ret;
        }
    }

    return NULL;
}

void pci_destroy_device(pci_device_t* dev) {
    free(dev);
}

static pci_found_t* _find_by_bsf(u8 bus, u8 slot, u8 func) {
    if (!pci_devices)
        return NULL;

    ll_foreach(node, pci_devices) {
        pci_found_t* dev = node->data;

        if (dev->bus == bus && dev->slot == slot && dev->func == func)
            return dev;
    }

    return NULL;
}

static void _write_legacy(u8 bus, u8 slot, u8 func, u8 offset, u32 value, u8 size) {
    pci_bus_write(bus, slot, func, offset, value, size);
}

static u32 _ecam_read(u64 base, u8 bus, u8 slot, u8 func, u16 offset, u8 size) {
    u64 phys = _ecam_addr(base, bus, slot, func);
    void* map = arch_phys_map(phys, 4096);

    if (!map)
        return 0xffffffffU;

    volatile u8* ptr = (volatile u8*)map + offset;
    u32 result;

    switch (size) {
    case 4:
        result = *(volatile u32*)ptr;
        break;
    case 2:
        result = *(volatile u16*)ptr;
        break;
    case 1:
        result = *(volatile u8*)ptr;
        break;
    default:
        result = 0xffffffffU;
        break;
    }

    arch_phys_unmap(map, 4096);
    return result;
}

static void _ecam_write(u64 base, u8 bus, u8 slot, u8 func, u16 offset, u32 value, u8 size) {
    u64 phys = _ecam_addr(base, bus, slot, func);
    void* map = arch_phys_map(phys, 4096);

    if (!map)
        return;

    volatile u8* ptr = (volatile u8*)map + offset;

    switch (size) {
    case 4:
        *(volatile u32*)ptr = value;
        break;
    case 2:
        *(volatile u16*)ptr = (u16)value;
        break;
    case 1:
        *(volatile u8*)ptr = (u8)value;
        break;
    }

    arch_phys_unmap(map, 4096);
}

pci_found_t* pci_find_node(u8 class, u8 subclass, pci_found_t* from) {
    list_node_t* start = pci_devices ? pci_devices->head : NULL;
    bool matched_from = (from == NULL);

    ll_foreach_from(node, start) {
        pci_found_t* dev = node->data;

        if (!matched_from) {
            if (dev == from)
                matched_from = true;

            continue;
        }

        if (dev->header.class == class && dev->header.subclass == subclass)
            return dev;
    }

    return NULL;
}

u32 pci_read_config(u8 bus, u8 slot, u8 func, u16 offset, u8 size) {
    pci_found_t* node = _find_by_bsf(bus, slot, func);

    if (node && node->base != (u64)-1)
        return _ecam_read(node->base, bus, slot, func, offset, size);

    return _read_legacy(bus, slot, func, (u8)offset, size);
}

void pci_write_config(u8 bus, u8 slot, u8 func, u16 offset, u32 value, u8 size) {
    pci_found_t* node = _find_by_bsf(bus, slot, func);

    if (node && node->base != (u64)-1)
        _ecam_write(node->base, bus, slot, func, offset, value, size);
    else
        _write_legacy(bus, slot, func, (u8)offset, value, size);
}

void pci_enable_bus_mastering(u8 bus, u8 slot, u8 func) {
    u16 cmd = (u16)pci_read_config(bus, slot, func, PCI_CFG_COMMAND, 2);
    cmd |= PCI_COMMAND_BUS_MASTER | PCI_COMMAND_MEM_SPACE;
    pci_write_config(bus, slot, func, PCI_CFG_COMMAND, cmd, 2);
}

u16 pci_find_capability(u8 bus, u8 slot, u8 func, u8 cap_id) {
    u16 status = (u16)pci_read_config(bus, slot, func, PCI_CFG_STATUS, 2);

    if (!(status & (1U << 4))) /* Capabilities List bit */
        return 0;

    u8 ptr = (u8)(pci_read_config(bus, slot, func, PCI_CFG_CAP_PTR, 1) & 0xfc);

    for (size_t i = 0; i < 48 && ptr; i++) {
        u8 id = (u8)pci_read_config(bus, slot, func, ptr, 1);

        if (id == cap_id)
            return ptr;

        ptr = (u8)(pci_read_config(bus, slot, func, ptr + 1, 1) & 0xfc);
    }

    return 0;
}

bool pci_enable_msi(u8 bus, u8 slot, u8 func, u8 vector, u32 lapic_dest) {
    u16 cap = pci_find_capability(bus, slot, func, PCI_CAP_MSI);

    if (!cap)
        return false;

    u16 msg_ctrl = (u16)pci_read_config(bus, slot, func, cap + 2, 2);
    bool is_64bit = (msg_ctrl & (1U << 7)) != 0;

    /* Disable MSI while programming */
    pci_write_config(bus, slot, func, cap + 2, msg_ctrl & ~1U, 2);

    /* Message Address: 0xFEE00000 | (LAPIC_ID << 12) */
    u32 addr_lo = 0xFEE00000U | (lapic_dest << 12);
    pci_write_config(bus, slot, func, cap + 4, addr_lo, 4);

    u16 data_offset;

    if (is_64bit) {
        pci_write_config(bus, slot, func, cap + 8, 0, 4); /* upper address = 0 */
        data_offset = cap + 12;
    } else {
        data_offset = cap + 8;
    }

    /* Message Data: vector number, fixed delivery */
    pci_write_config(bus, slot, func, data_offset, vector, 2);

    /* Request single message, enable MSI */
    msg_ctrl &= ~(0x70U); /* clear Multiple Message Enable */
    msg_ctrl |= 1U;       /* MSI Enable */
    pci_write_config(bus, slot, func, cap + 2, msg_ctrl, 2);

    /* Disable legacy INTx */
    u16 cmd = (u16)pci_read_config(bus, slot, func, PCI_CFG_COMMAND, 2);
    cmd |= PCI_COMMAND_INT_DIS;
    pci_write_config(bus, slot, func, PCI_CFG_COMMAND, cmd, 2);

    return true;
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

void dump_pci_devices(void) {
    if (!pci_devices)
        return;

    log_debug("pci: dump of detected %s devices:", pci_is_express ? "PCIE" : "PCI");

    ll_foreach(node, pci_devices) {
        pci_found_t* dev = node->data;

        log_debug(
            "[ subclass: %.2x prog_if: %.2x ] %s",
            dev->header.subclass,
            dev->header.prog_if,
            pci_stringify_class(dev->header.class)
        );
    }
}
