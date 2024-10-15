#include "acpi.h"

#include <base/addr.h>
#include <base/types.h>
#include <data/list.h>
#include <log/log.h>
#include <string.h>

#include "mem/heap.h"

static linked_list* tables;


static bool _validate_header(sdt_header* header) {
    u8* header_bytes = (u8*)header;
    u8 sum = 0;

    for (usize i = 0; i < header->length; i++)
        sum += header_bytes[i];

    return (sum & 0xff) == 0;
}

static void _append_table(sdt_header* header) {
    void* table_cpy = kcalloc(header->length);
    memcpy(table_cpy, header, header->length);

    list_append(tables, list_create_node(table_cpy));
}


static void parse_rsdp(rsdt* rsdt_ptr) {
    usize entries = (rsdt_ptr->header.length - sizeof(sdt_header)) / 4;

    log_debug("Dump of RSDT tables:");
    for (usize i = 0; i < entries; i++) {
        sdt_header* header = (sdt_header*)(uptr)ID_MAPPED_VADDR(rsdt_ptr->table_ptrs[i]);

        if (!_validate_header(header)) {
            log_error("RSDP has invalid entry at index %zd", i);
            continue;
        }

        _append_table(header);

        log_debug("[ id: %.4s oem: %.6s ]", header->signature, header->oem_id);
    }
}

static void parse_xsdp(xsdt* xsdt_ptr) {
    usize entries = (xsdt_ptr->header.length - sizeof(sdt_header)) / 8;

    log_debug("Dump of XSDT tables:");
    for (usize i = 0; i < entries; i++) {
        sdt_header* header = (sdt_header*)ID_MAPPED_VADDR(xsdt_ptr->table_ptrs[i]);

        if (!_validate_header(header)) {
            log_error("XSDP has invalid entry at index %zd", i);
            continue;
        }

        _append_table(header);

        log_debug("[ id: %.4s oem: %.6s ]", header->signature, header->oem_id);
    }
}


void acpi_init(u64 ptr) {
    if (!ptr) {
        log_error("RSDP table not found!");
        return;
    }

    tables = list_create();
    rsdp* table = (rsdp*)ID_MAPPED_VADDR(ptr);

    if (table->revision >= 2)
        parse_xsdp((xsdt*)(uptr)ID_MAPPED_VADDR(table->xsdt_addr));
    else
        parse_rsdp((rsdt*)(uptr)ID_MAPPED_VADDR(table->rsdt_addr));
}


sdt_header* acpi_find_table(char id[4]) {
    foreach (node, tables) {
        sdt_header* header = (sdt_header*)node->data;

        if (!memcmp(&header->signature, id, 4))
            return header;
    }

    return NULL;
}
