#include "acpi.h"

#include <arch/arch.h>
#include <data/list.h>
#include <log/log.h>
#include <stdlib.h>
#include <string.h>

static linked_list_t *acpi_tables = NULL;
static bool acpi_xsdt = false;

static bool _checksum(const void *table, size_t length) {
    const u8 *bytes = table;
    u8 sum = 0;

    for (size_t i = 0; i < length; i++) {
        sum += bytes[i];
    }

    return !(sum & 0xff);
}

static sdt_header_t *_copy_table(u64 phys_addr) {
    sdt_header_t header = {0};
    void *header_map = arch_phys_map(phys_addr, sizeof(sdt_header_t), 0);

    if (!header_map) {
        return NULL;
    }

    memcpy(&header, header_map, sizeof(sdt_header_t));
    arch_phys_unmap(header_map, sizeof(sdt_header_t));

    if (header.length < sizeof(sdt_header_t)) {
        return NULL;
    }

    void *table_map = arch_phys_map(phys_addr, header.length, 0);
    if (!table_map) {
        return NULL;
    }

    if (!_checksum(table_map, header.length)) {
        log_warn("invalid checksum for %.4s", header.signature);
        arch_phys_unmap(table_map, header.length);
        return NULL;
    }

    sdt_header_t *copy = malloc(header.length);
    if (!copy) {
        arch_phys_unmap(table_map, header.length);
        return NULL;
    }

    memcpy(copy, table_map, header.length);
    arch_phys_unmap(table_map, header.length);

    return copy;
}

static void _append_table(sdt_header_t *table) {
    if (!table || !acpi_tables) {
        return;
    }

    list_append(acpi_tables, list_create_node(table));
}

static void _parse_rsdt(u64 rsdt_phys) {
    sdt_header_t *table = _copy_table(rsdt_phys);
    if (!table) {
        return;
    }

    _append_table(table);

    rsdt_t *rsdt = (rsdt_t *)table;
    size_t entries =
        (rsdt->header.length - sizeof(sdt_header_t)) / sizeof(rsdt->table_ptrs[0]);

    for (size_t i = 0; i < entries; i++) {
        sdt_header_t *entry = _copy_table(rsdt->table_ptrs[i]);

        if (!entry) {
            log_warn("RSDT entry %zu invalid", i);
            continue;
        }

        _append_table(entry);
    }
}

static void _parse_xsdt(u64 xsdt_phys) {
    acpi_xsdt = true;

    sdt_header_t *table = _copy_table(xsdt_phys);
    if (!table) {
        return;
    }

    _append_table(table);

    xsdt_t *xsdt = (xsdt_t *)table;
    size_t entries =
        (xsdt->header.length - sizeof(sdt_header_t)) / sizeof(xsdt->table_ptrs[0]);

    for (size_t i = 0; i < entries; i++) {
        sdt_header_t *entry = _copy_table(xsdt->table_ptrs[i]);

        if (!entry) {
            log_warn("XSDT entry %zu invalid", i);
            continue;
        }

        _append_table(entry);
    }
}

static bool _rsdp_validate(rsdp_t *rsdp) {
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0) {
        return false;
    }

    if (!_checksum(rsdp, 20)) {
        return false;
    }

    if (rsdp->revision >= 2 && rsdp->length >= sizeof(rsdp_t)) {
        return _checksum(rsdp, rsdp->length);
    }

    return true;
}

void acpi_init(u64 rsdp_ptr) {
    if (!rsdp_ptr) {
        log_warn("RSDP not provided");
        return;
    }

    acpi_tables = list_create();
    if (!acpi_tables) {
        return;
    }

    rsdp_t rsdp = {0};
    void *rsdp_map = arch_phys_map(rsdp_ptr, sizeof(rsdp_t), 0);

    if (!rsdp_map) {
        return;
    }

    memcpy(&rsdp, rsdp_map, sizeof(rsdp_t));
    arch_phys_unmap(rsdp_map, sizeof(rsdp_t));

    if (!_rsdp_validate(&rsdp)) {
        log_warn("invalid RSDP");
        return;
    }

    if (rsdp.revision >= 2 && rsdp.xsdt_addr) {
        _parse_xsdt(rsdp.xsdt_addr);
    } else {
        _parse_rsdt(rsdp.rsdt_addr);
    }

    log_info(
        "loaded %zu %s tables", acpi_tables->length, acpi_xsdt ? "XSDT" : "RSDT"
    );
}

sdt_header_t *acpi_find_table(char id[4]) {
    if (!acpi_tables) {
        return NULL;
    }

    ll_foreach(node, acpi_tables) {
        sdt_header_t *header = node->data;

        if (header && !memcmp(header->signature, id, 4)) {
            return header;
        }
    }

    return NULL;
}

void dump_acpi_tables(void) {
    if (!acpi_tables) {
        return;
    }

    log_debug("dump of %s tables:", acpi_xsdt ? "XSDT" : "RSDT");

    ll_foreach(node, acpi_tables) {
        sdt_header_t *header = node->data;

        if (!header) {
            continue;
        }

        log_debug("[ id: %.4s oem: %.6s ]", header->signature, header->oem_id);
    }
}
