#include <arch/pci.h>
#include <x86/asm.h>

#define PCI_CONFIG_ADDRESS_PORT 0xcf8
#define PCI_CONFIG_DATA_PORT    0xcfc

#define PCI_CONFIG_ENABLE_BIT 0x80000000U
#define PCI_OFFSET_DWORD_MASK 0xfcU

#define PCI_DATA_WORD_OFFSET_MASK 0x2U
#define PCI_DATA_BYTE_OFFSET_MASK 0x3U

#define PCI_INVALID_READ_VALUE 0xffffffffU


u32 pci_bus_read(u8 bus, u8 slot, u8 func, u8 offset, u8 size) {
    u32 cfg_addr = PCI_CONFIG_ENABLE_BIT;

    cfg_addr |= (u32)bus << 16;
    cfg_addr |= (u32)slot << 11;
    cfg_addr |= (u32)func << 8;
    cfg_addr |= (u32)offset & PCI_OFFSET_DWORD_MASK;

    outl(PCI_CONFIG_ADDRESS_PORT, cfg_addr);

    switch (size) {
    case 4:
        return inl(PCI_CONFIG_DATA_PORT);
    case 2: {
        u16 data_port =
            PCI_CONFIG_DATA_PORT + (offset & PCI_DATA_WORD_OFFSET_MASK);

        return inw(data_port);
    }
    case 1: {
        u16 data_port = 
            PCI_CONFIG_DATA_PORT + (offset & PCI_DATA_BYTE_OFFSET_MASK);

        return inb(data_port);
    }
    default:
        return PCI_INVALID_READ_VALUE;
    }
}

void pci_bus_write(u8 bus, u8 slot, u8 func, u8 offset, u32 value, u8 size) {
    u32 cfg_addr = PCI_CONFIG_ENABLE_BIT;

    cfg_addr |= (u32)bus << 16;
    cfg_addr |= (u32)slot << 11;
    cfg_addr |= (u32)func << 8;
    cfg_addr |= (u32)offset & PCI_OFFSET_DWORD_MASK;

    outl(PCI_CONFIG_ADDRESS_PORT, cfg_addr);

    switch (size) {
    case 4:
        outl(PCI_CONFIG_DATA_PORT, value);
        break;
    case 2: {
        u16 data_port =
            PCI_CONFIG_DATA_PORT + (offset & PCI_DATA_WORD_OFFSET_MASK);

        outw(data_port, (u16)value);
        break;
    }
    case 1: {
        u16 data_port =
            PCI_CONFIG_DATA_PORT + (offset & PCI_DATA_BYTE_OFFSET_MASK);

        outb(data_port, (u8)value);
        break;
    }
    }
}

bool pci_ecam_addr_supported(u64 addr) {
#if defined(__i386__)
    return addr < 0x100000000ULL;
#else
    (void)addr;
    return true;
#endif
}
