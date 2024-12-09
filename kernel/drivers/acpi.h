#pragma once

#include <base/attributes.h>
#include <base/types.h>

// https://wiki.osdev.org/RSDP
typedef struct PACKED {
    char signature[8];
    u8 checksum;
    char oem_id[6];
    u8 revision;
    u32 rsdt_addr;

    // Versions >= 2.0 use the fields bellow
    u32 length;
    u64 xsdt_addr;
    u8 checksum_extended;
    u8 _reserved0[3];
} rsdp;

// https://wiki.osdev.org/RSDT
typedef struct PACKED {
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} sdt_header;

typedef struct PACKED {
    sdt_header header;
    u64 table_ptrs[];
} xsdt;

typedef struct PACKED {
    sdt_header header;
    u32 table_ptrs[];
} rsdt;

typedef struct PACKED {
    sdt_header header;
    u32 local_apic_addr;
    u32 flags;
    u8 entries[];
} madt;

typedef struct PACKED {
    u8 type;
    u8 length;
    u8 data[];
} madt_entry;

enum madt_entry_type {
    MT_LOC_APIC = 0,
    MT_IO_APIC = 1,
    MT_IO_APIC_INT_SRC = 2,
    MT_IO_APIC_NMINT_SRC = 3,
    MT_LOC_APIC_NMINT = 4,
    MT_LOC_APIC_ADDR = 5,
    MT_LOC_APIC2 = 9,
};

typedef struct PACKED {
    u64 base_addr;
    u16 pci_seg_group;
    u8 start_bus;
    u8 end_bus;
    u32 _reserved0;
} mcfg_entry;

typedef struct PACKED {
    sdt_header header;
    u64 _reserved0;
    mcfg_entry entries[];
} mcfg;


void acpi_init(u64 rsdp_ptr);

sdt_header* acpi_find_table(char id[4]);

void dump_acpi_tables(void);
