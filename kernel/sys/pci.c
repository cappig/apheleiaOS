#include "pci.h"

#include <arch/arch.h>
#include <arch/pci.h>
#include <data/hashmap.h>
#include <data/list.h>
#include <inttypes.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>
#include <sys/panic.h>

#include "acpi.h"

static bool pci_is_express = false;
static linked_list_t *pci_devices = NULL;
static hashmap_t *pci_bsf_index = NULL;

#define PCI_ECAM_WINDOW_MAX 64

typedef struct {
    u64 base;
    u8 start_bus;
    u8 end_bus;
    u16 segment;
} pci_ecam_window_t;

static pci_ecam_window_t pci_ecam_windows[PCI_ECAM_WINDOW_MAX];
static size_t pci_ecam_window_count = 0;


static u64 _bsf_key(u8 bus, u8 slot, u8 func) {
    return ((u64)bus << 16) | ((u64)slot << 8) | (u64)func;
}

static void _index_device(pci_found_t *device) {
    if (!pci_bsf_index || !device) {
        return;
    }

    if (
        !hashmap_set(
            pci_bsf_index,
            _bsf_key(device->bus, device->slot, device->func),
            (u64)(uintptr_t)device
        )
    ) {
        panic("pci registry index insert failed");
    }
}

static bool _device_exists(u8 bus, u8 slot, u8 func) {
    if (pci_bsf_index) {
        u64 encoded = 0;
        if (hashmap_get(pci_bsf_index, _bsf_key(bus, slot, func), &encoded)) {
            return true;
        }
    }

    if (!pci_devices) {
        return false;
    }

    ll_foreach(node, pci_devices) {
        pci_found_t *dev = node->data;
        if (!dev) {
            continue;
        }

        if (dev->bus == bus && dev->slot == slot && dev->func == func) {
            return true;
        }
    }

    return false;
}

static bool _append_device(pci_found_t *device) {
    if (!device || !pci_devices) {
        return false;
    }

    if (_device_exists(device->bus, device->slot, device->func)) {
        free(device);
        return true;
    }

    list_node_t *node = list_create_node(device);
    if (!node) {
        free(device);
        return false;
    }

    list_append(pci_devices, node);
    _index_device(device);
    return true;
}

static u64 _ecam_addr(u64 base, u8 bus, u8 slot, u8 func) {
    return base + ((u64)bus << 20) + ((u64)slot << 15) + ((u64)func << 12);
}

static void _clear_ecam_windows(void) {
    pci_ecam_window_count = 0;
    memset(pci_ecam_windows, 0, sizeof(pci_ecam_windows));
}

static void _register_ecam_window(
    u64 ecam_base,
    u16 segment,
    u8 start_bus,
    u8 end_bus
) {
    if (pci_ecam_window_count >= ARRAY_LEN(pci_ecam_windows)) {
        return;
    }

    pci_ecam_windows[pci_ecam_window_count++] = (pci_ecam_window_t){
        .base = ecam_base,
        .start_bus = start_bus,
        .end_bus = end_bus,
        .segment = segment,
    };
}

static bool _ecam_base_for_bus(u8 bus, u64 *out_base) {
    if (!out_base) {
        return false;
    }

    for (size_t i = 0; i < pci_ecam_window_count; i++) {
        const pci_ecam_window_t *win = &pci_ecam_windows[i];
        if (bus < win->start_bus || bus > win->end_bus) {
            continue;
        }

        *out_base = win->base;
        return true;
    }

    return false;
}

static u32 _read_legacy(u8 bus, u8 slot, u8 func, u8 offset, size_t bytes) {
    return pci_bus_read(bus, slot, func, offset, bytes);
}

static bool
_ecam_read_header(u64 base, u8 bus, u8 slot, u8 func, pci_header_t *out) {
    if (!out) {
        return false;
    }

    u64 phys = _ecam_addr(base, bus, slot, func);
    void *map = arch_phys_map(phys, sizeof(pci_header_t), PHYS_MAP_MMIO);

    if (!map) {
        return false;
    }

    memcpy(out, map, sizeof(pci_header_t));
    arch_phys_unmap(map, sizeof(pci_header_t));

    return true;
}

static void _probe_slot_legacy(u8 bus, u8 slot) {
    u8 type =
        (u8)_read_legacy(bus, slot, 0, offsetof(pci_header_t, header_type), 1);

    size_t funcs = (type & 0x80) ? 8 : 1;

    for (size_t func = 0; func < funcs; func++) {
        u16 vendor = (u16)_read_legacy(
            bus, slot, func, offsetof(pci_header_t, vendor_id), 2
        );

        if (vendor == PCI_NONE) {
            continue;
        }

        if (_device_exists(bus, slot, (u8)func)) {
            continue;
        }

        u64 ecam_base = 0;
        bool ecam_match = false;

        if (_ecam_base_for_bus(bus, &ecam_base)) {
            pci_header_t ecam_header = {0};

            if (_ecam_read_header(ecam_base, bus, slot, (u8)func, &ecam_header)) {
                ecam_match = (ecam_header.vendor_id == vendor);
            }
        }

        pci_found_t *device = calloc(1, sizeof(pci_found_t));
        if (!device) {
            return;
        }

        device->base = ecam_match ? ecam_base : ~0ULL;
        device->bus = bus;
        device->slot = slot;
        device->func = (u8)func;

        u8 *header_bytes = (u8 *)&device->header;

        for (size_t i = 0; i < sizeof(pci_header_t); i++) {
            header_bytes[i] = (u8)_read_legacy(bus, slot, func, (u8)i, 1);
        }

        if (!_append_device(device)) {
            return;
        }
    }
}

static void _probe_slot_express(u64 base, u8 bus, u8 slot) {
    pci_header_t header = {0};

    if (!_ecam_read_header(base, bus, slot, 0, &header)) {
        return;
    }

    if (header.vendor_id == PCI_NONE) {
        return;
    }

    size_t funcs = (header.header_type & 0x80) ? 8 : 1;

    for (size_t func = 0; func < funcs; func++) {
        pci_header_t current = {0};

        if (!_ecam_read_header(base, bus, slot, (u8)func, &current)) {
            continue;
        }

        if (current.vendor_id == PCI_NONE) {
            continue;
        }

        if (_device_exists(bus, slot, (u8)func)) {
            continue;
        }

        pci_found_t *device = calloc(1, sizeof(pci_found_t));
        if (!device) {
            return;
        }

        device->base = base;
        device->bus = bus;
        device->slot = slot;
        device->func = (u8)func;
        device->header = current;

        if (!_append_device(device)) {
            return;
        }
    }
}

static void _init_legacy(void) {
    for (u16 bus = 0; bus < 256; bus++) {
        for (u8 slot = 0; slot < 32; slot++) {
            _probe_slot_legacy((u8)bus, slot);
        }
    }
}

static void _init_express(mcfg_t *table) {
    pci_is_express = true;

    if (!table || table->header.length < sizeof(mcfg_t)) {
        return;
    }

    size_t table_len = table->header.length - sizeof(mcfg_t);

    size_t count = table_len / sizeof(mcfg_entry_t);

    for (size_t i = 0; i < count; i++) {
        mcfg_entry_t *entry = &table->entries[i];

        if (!entry->base_addr) {
            continue;
        }

        if (entry->pci_seg_group != 0) {
            log_warn(
                "skipping unsupported MCFG segment %u (bus=%u..%u)",
                entry->pci_seg_group,
                entry->start_bus,
                entry->end_bus
            );
            continue;
        }

        if (!pci_ecam_addr_supported(entry->base_addr)) {
            log_warn("skipping MCFG entry above 4GiB");
            continue;
        }

        if (entry->start_bus > entry->end_bus) {
            continue;
        }

        u64 start_bus_off = (u64)entry->start_bus << 20;
        if (entry->base_addr < start_bus_off) {
            log_warn(
                "skipping invalid MCFG entry seg=%u bus=%u..%u base=%#" PRIx64,
                entry->pci_seg_group,
                entry->start_bus,
                entry->end_bus,
                entry->base_addr
            );
            continue;
        }

        // ECAM base in MCFG entries is anchored to start_bus;
        // convert to a synthetic bus-0 base so _ecam_addr() can use absolute bus
        u64 ecam_base = entry->base_addr - start_bus_off;
        _register_ecam_window(
            ecam_base,
            entry->pci_seg_group,
            entry->start_bus,
            entry->end_bus
        );

        log_debug(
            "MCFG seg=%u bus=%u..%u base=%#" PRIx64,
            entry->pci_seg_group,
            entry->start_bus,
            entry->end_bus,
            entry->base_addr
        );

        for (u16 bus = entry->start_bus; bus <= entry->end_bus; bus++) {
            for (u8 slot = 0; slot < 32; slot++) {
                _probe_slot_express(ecam_base, (u8)bus, slot);
            }
        }
    }
}

size_t pci_init(void) {
    pci_devices = list_create();
    if (!pci_devices) {
        return 0;
    }
    pci_bsf_index = hashmap_create();
    _clear_ecam_windows();

    mcfg_t *table = (mcfg_t *)acpi_find_table("MCFG");

    if (table) {
        size_t before = pci_devices->length;
        _init_express(table);

        size_t found = pci_devices->length - before;
        if (found <= 1) {
            log_warn(
                "MCFG probe found only %zu device%s; falling back to legacy PCI scan",
                found,
                found == 1 ? "" : "s"
            );
            _init_legacy();
        }
    } else {
        _init_legacy();
    }

    log_info(
        "detected %zu devices on the %s bus",
        pci_devices->length,
        pci_is_express ? "PCIE" : "PCI"
    );

    return pci_devices->length;
}

// NOTE: returns the conventional 256 bytes, not the full 4096 bytes for PCIe
pci_device_t *pci_find_device(u8 class, u8 subclass, pci_device_t *from) {
    list_node_t *start = pci_devices ? pci_devices->head : NULL;
    bool matched_from = (from == NULL);

    ll_foreach_from(node, start) {
        pci_found_t *dev = node->data;
        pci_header_t *header = &dev->header;

        if (!matched_from) {
            if (
                header->vendor_id == from->header.vendor_id &&
                header->device_id == from->header.device_id &&
                header->class == from->header.class &&
                header->subclass == from->header.subclass &&
                header->prog_if == from->header.prog_if
            ) {
                matched_from = true;
            }

            continue;
        }

        if (header->class == class && header->subclass == subclass) {
            u8 *ret = malloc(256);
            if (!ret) {
                return NULL;
            }

            if (dev->base != (u64)-1) {
                u64 phys =
                    _ecam_addr(dev->base, dev->bus, dev->slot, dev->func);

                void *map = arch_phys_map(phys, 256, PHYS_MAP_MMIO);

                if (!map) {
                    free(ret);
                    return NULL;
                }

                memcpy(ret, map, 256);
                arch_phys_unmap(map, 256);
            } else {
                for (size_t i = 0; i < 256; i++) {
                    ret[i] = (u8)_read_legacy(
                        dev->bus, dev->slot, dev->func, (u8)i, 1
                    );
                }
            }

            return (pci_device_t *)ret;
        }
    }

    return NULL;
}

void pci_destroy_device(pci_device_t *dev) {
    free(dev);
}

static pci_found_t *_find_by_bsf(u8 bus, u8 slot, u8 func) {
    if (!pci_devices) {
        return NULL;
    }

    if (pci_bsf_index) {
        u64 encoded = 0;

        if (hashmap_get(pci_bsf_index, _bsf_key(bus, slot, func), &encoded)) {
            pci_found_t *device = (pci_found_t *)(uintptr_t)encoded;

            if (device && device->bus == bus && device->slot == slot && device->func == func) {
                return device;
            }
        }
    }

    ll_foreach(node, pci_devices) {
        pci_found_t *dev = node->data;

        if (dev->bus == bus && dev->slot == slot && dev->func == func) {
            return dev;
        }
    }

    return NULL;
}

static void
_write_legacy(u8 bus, u8 slot, u8 func, u8 offset, u32 value, u8 size) {
    pci_bus_write(bus, slot, func, offset, value, size);
}

static u32 _ecam_read(u64 base, u8 bus, u8 slot, u8 func, u16 offset, u8 size) {
    u64 phys = _ecam_addr(base, bus, slot, func);
    void *map = arch_phys_map(phys, 4096, PHYS_MAP_MMIO);

    if (!map) {
        return 0xffffffffU;
    }

    volatile u8 *ptr = (volatile u8 *)map + offset;
    u32 result;

    switch (size) {
    case 4:
        result = *(volatile u32 *)ptr;
        break;
    case 2:
        result = *(volatile u16 *)ptr;
        break;
    case 1:
        result = *(volatile u8 *)ptr;
        break;
    default:
        result = 0xffffffffU;
        break;
    }

    arch_phys_unmap(map, 4096);
    return result;
}

static void _ecam_write(
    u64 base,
    u8 bus,
    u8 slot,
    u8 func,
    u16 offset,
    u32 value,
    u8 size
) {
    u64 phys = _ecam_addr(base, bus, slot, func);
    void *map = arch_phys_map(phys, 4096, PHYS_MAP_MMIO);

    if (!map) {
        return;
    }

    volatile u8 *ptr = (volatile u8 *)map + offset;

    switch (size) {
    case 4:
        *(volatile u32 *)ptr = value;
        break;
    case 2:
        *(volatile u16 *)ptr = (u16)value;
        break;
    case 1:
        *(volatile u8 *)ptr = (u8)value;
        break;
    }

    arch_phys_unmap(map, 4096);
}

pci_found_t *pci_find_node(u8 class, u8 subclass, pci_found_t *from) {
    list_node_t *start = pci_devices ? pci_devices->head : NULL;
    bool matched_from = (from == NULL);

    ll_foreach_from(node, start) {
        pci_found_t *dev = node->data;

        if (!matched_from) {
            if (dev == from) {
                matched_from = true;
            }

            continue;
        }

        if (dev->header.class == class && dev->header.subclass == subclass) {
            return dev;
        }
    }

    return NULL;
}

u32 pci_read_config(u8 bus, u8 slot, u8 func, u16 offset, u8 size) {
    pci_found_t *node = _find_by_bsf(bus, slot, func);

    if (node && node->base != (u64)-1) {
        return _ecam_read(node->base, bus, slot, func, offset, size);
    }

    return _read_legacy(bus, slot, func, (u8)offset, size);
}

void pci_write_config(
    u8 bus,
    u8 slot,
    u8 func,
    u16 offset,
    u32 value,
    u8 size
) {
    pci_found_t *node = _find_by_bsf(bus, slot, func);

    if (node && node->base != (u64)-1) {
        _ecam_write(node->base, bus, slot, func, offset, value, size);
    } else {
        _write_legacy(bus, slot, func, (u8)offset, value, size);
    }
}

void pci_enable_bus_mastering(u8 bus, u8 slot, u8 func) {
    u16 cmd = (u16)pci_read_config(bus, slot, func, PCI_CFG_COMMAND, 2);
    cmd |= PCI_COMMAND_BUS_MASTER | PCI_COMMAND_MEM_SPACE;
    pci_write_config(bus, slot, func, PCI_CFG_COMMAND, cmd, 2);
}

u16 pci_find_capability(u8 bus, u8 slot, u8 func, u8 cap_id) {
    u16 status = (u16)pci_read_config(bus, slot, func, PCI_CFG_STATUS, 2);

    if (!(status & (1U << 4))) {
        return 0;
    }

    u8 ptr = (u8)(pci_read_config(bus, slot, func, PCI_CFG_CAP_PTR, 1) & 0xfc);

    for (size_t i = 0; i < 48 && ptr; i++) {
        u8 id = (u8)pci_read_config(bus, slot, func, ptr, 1);

        if (id == cap_id) {
            return ptr;
        }

        ptr = (u8)(pci_read_config(bus, slot, func, ptr + 1, 1) & 0xfc);
    }

    return 0;
}

bool pci_enable_msi(u8 bus, u8 slot, u8 func, u8 vector, u32 lapic_dest) {
    u16 cap = pci_find_capability(bus, slot, func, PCI_CAP_MSI);

    if (!cap) {
        return false;
    }

    u16 msg_ctrl = (u16)pci_read_config(bus, slot, func, cap + 2, 2);
    bool is_64bit = (msg_ctrl & (1U << 7)) != 0;

    // Disable MSI while programming
    pci_write_config(bus, slot, func, cap + 2, msg_ctrl & ~1U, 2);

    u32 addr_lo = 0xFEE00000U | (lapic_dest << 12);
    pci_write_config(bus, slot, func, cap + 4, addr_lo, 4);

    u16 data_offset;

    if (is_64bit) {
        pci_write_config(bus, slot, func, cap + 8, 0, 4);
        data_offset = cap + 12;
    } else {
        data_offset = cap + 8;
    }

    // Message Data: vector number, fixed delivery
    pci_write_config(bus, slot, func, data_offset, vector, 2);

    // Request single message, enable MSI
    msg_ctrl &= ~(0x70U);
    msg_ctrl |= 1U;
    pci_write_config(bus, slot, func, cap + 2, msg_ctrl, 2);

    // Disable legacy INTx
    u16 cmd = (u16)pci_read_config(bus, slot, func, PCI_CFG_COMMAND, 2);
    cmd |= PCI_COMMAND_INT_DIS;
    pci_write_config(bus, slot, func, PCI_CFG_COMMAND, cmd, 2);

    return true;
}

bool pci_disable_msi(u8 bus, u8 slot, u8 func) {
    bool had_msi = false;
    u16 cap = pci_find_capability(bus, slot, func, PCI_CAP_MSI);

    if (cap) {
        u16 msg_ctrl = (u16)pci_read_config(bus, slot, func, cap + 2, 2);
        if (msg_ctrl & 1U) {
            had_msi = true;
            msg_ctrl &= ~1U;
            pci_write_config(bus, slot, func, cap + 2, msg_ctrl, 2);
        }
    }

    u16 cmd = (u16)pci_read_config(bus, slot, func, PCI_CFG_COMMAND, 2);
    cmd &= (u16)~PCI_COMMAND_INT_DIS;
    pci_write_config(bus, slot, func, PCI_CFG_COMMAND, cmd, 2);

    return had_msi;
}

const char *pci_stringify_class(u8 class) {
    if (class <= 0x13) {
        return pci_class_strings[class];
    }

    if (class == 0x40) {
        return pci_class_strings[21];
    }

    if (class == 0xff) {
        return pci_class_strings[22];
    }

    return pci_class_strings[20];
}

void dump_pci_devices(void) {
    if (!pci_devices) {
        return;
    }

    log_debug("dump of detected %s devices:", pci_is_express ? "PCIE" : "PCI");

    ll_foreach(node, pci_devices) {
        pci_found_t *dev = node->data;

        log_debug(
            "[ %u:%u.%u ven=%#06x dev=%#06x class=%#04x subclass=%#04x prog_if=%#04x ] %s",
            dev->bus,
            dev->slot,
            dev->func,
            dev->header.vendor_id,
            dev->header.device_id,
            dev->header.class,
            dev->header.subclass,
            dev->header.prog_if,
            pci_stringify_class(dev->header.class)
        );
    }
}
