#pragma once

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <stddef.h>

#define PCI_ADDRESS 0xcf8
#define PCI_VALUE   0xcfc

#define PCI_NONE 0xffff

#define PCI_REG(name)      (offsetof(pci_header, name))
#define PCI_REG_SIZE(name) (SIZEOF_MEMBER(pci_header, name))

// The PCIE SIG keeps the official PCI express standard _and the drafts as well_ behind a massive
// paywall so we have to rely on wikis and "leaked PDFs"
// https://wiki.osdev.org/PCI_Express
// https://wiki.osdev.org/PCI

typedef struct PACKED {
    u16 vendor_id;
    u16 device_id;
    u16 command;
    u16 status;
    u8 revision_id;
    u8 prog_if;
    u8 subclass;
    u8 class;
    u8 cache_line_size;
    u8 latency_timer;
    u8 header_type;
    u8 BIST;
} pci_header;

typedef struct PACKED {
    u32 bar0, bar1, bar2, bar3, bar4, bar5;
    u32 cardbus_cis_ptr;
    u16 subsystem_vendor_id;
    u16 subsystem_id;
    u32 expansion_rom_base;
    u8 capability_ptr;
    u8 _reserved0[7];
    u8 int_line;
    u8 int_pin;
    u8 min_grant;
    u8 max_latency;
} pci_generic;

typedef struct PACKED {
    u32 bar0, bar1;
    u8 primary_bus_num;
    u8 secondary_bus_num;
    u8 subordinate_bus_num;
    u8 secondary_latency_timer;
    u8 io_base;
    u8 io_limit;
    u16 secondary_status;
    u16 mem_base;
    u16 mem_limit;
    u16 prefetch_mem_base;
    u16 prefetch_mem_limit;
    u32 prefetch_base_upper;
    u16 io_base_upper;
    u16 io_limit_upper;
    u8 capability_ptr;
    u8 _reserved0[3];
    u32 expansion_rom_base;
    u8 int_line;
    u8 int_pin;
    u16 bridge_control;
} pci_bridge;

// This struct is 256 bytes long under conventional PCI
// and 4096 bytes long under PCI express
typedef struct PACKED {
    pci_header header;

    union {
        pci_generic generic;
        pci_bridge bridge;
        // A PCI to cardbus bridge is also defined but
        // cardbus is obsolete since 2003 so I just didn't bother
    };
} pci_device;

// TODO: add a string table so that we can print nicer messages
enum pci_class {
    PCI_UNCLASSIFIED = 0x00,
    PCI_MASS_STORAGE = 0x01,
    PCI_NETWORK = 0x02,
    PCI_DISPLAY = 0x03,
    PCI_MULTIMEDIA = 0x04,
    PCI_MEMORY = 0x05,
    PCI_BRIDGE = 0x06,
    PCI_SIMPLE_COMMUNICATION_CONTROLLER = 0x07,
    PCI_BASE_SYSTEM_PERIPHERAL = 0x08,
    PCI_INPUT_DEVICE = 0x09,
    PCI_DOCKING_STATION = 0x0a,
    PCI_PROCESSOR = 0x0b,
    PCI_SERIAL_BUS = 0x0c,
    PCI_WIRELESS = 0x0d,
    PCI_INTELLIGENT_CONTROLLER = 0x0e,
    PCI_SATELLITE_COMMUNICATION = 0x0f,
    PCI_ENCRYPTION_CONTROLLER = 0x10,
    PCI_SIGNAL_PROCESSING_CONTROLLER = 0x11,
    PCI_PROCESSING_ACCELERATOR = 0x12,
    PCI_NON_ESSENTIAL_INSTRUMENTATION = 0x13,
    // 0x14 - 0x3f = reserved
    PCI_COPROCESSOR = 0x40,
    // 0x41 - 0xfe = reserved
    PCI_UNASSIGNED = 0xff, // vendor specific
};

MAYBE_UNUSED
static const char* pci_class_strings[] = {
    "Unclassified",
    "Mass Storage Controller",
    "Network Controller",
    "Display Controller",
    "Multimedia Controller",
    "Memory Controller",
    "Bridge",
    "Simple Communication Controller",
    "Base System Peripheral",
    "Input Device Controller",
    "Docking Station",
    "Processor",
    "Serial Bus Controller",
    "Wireless Controller",
    "Intelligent Controller",
    "Satellite Communication Controller",
    "Encryption Controller",
    "Signal Processing Controller",
    "Processing Accelerator",
    "Non-Essential Instrumentation",
    "?", // ...a bunch of reserved space here
    "Co-Processor", // 0x40
    "Unassigned (Vendor specific)", // 0xff
};

enum pci_mass_storage_subclass {
    PCI_MS_SCSI_BUS = 0x00,
    PCI_MS_IDE = 0x01,
    PCI_MS_FLOPPY = 0x02,
    PCI_MS_IPI_BUS = 0x03,
    PCI_MS_RAID = 0x04,
    PCI_MS_ATA = 0x05,
    PCI_MS_SATA = 0x06,
    PCI_MS_SAS = 0x07,
    PCI_MS_NVM = 0x08,
    PCI_MS_OTHER = 0x80,
};

typedef struct {
    u64 base; // ~0 if conventional PCI
    u8 bus;
    u8 slot;
    u8 func;
    pci_header header;
} pci_found;


usize pci_init(void);

void dump_pci_devices(void);
const char* pci_stringify_class(u8 class);

pci_device* pci_find_device(u8 class, u8 subclass, pci_device* from);
void pci_destroy_device(pci_device* dev);
