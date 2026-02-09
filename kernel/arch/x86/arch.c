#include <log/log.h>
#include <sys/acpi.h>
#include <sys/panic.h>
#include <sys/pci.h>
#include <x86/asm.h>
#include <x86/ata.h>
#include <x86/boot.h>
#include <x86/gdt.h>
#include <x86/idt.h>
#include <x86/mm/heap.h>
#include <x86/mm/physical.h>
#include <x86/pic.h>
#include <x86/serial.h>

static void _serial_puts(const char* s) {
    send_serial_string(SERIAL_COM1, s);
}

static void _select_log_level(const boot_info_t* info) {
    if (!info)
        return;

    switch (info->args.debug) {
    case DEBUG_NONE:
        log_set_lvl(LOG_INFO);
        break;
    case DEBUG_MINIMAL:
        log_set_lvl(LOG_INFO);
        break;
    case DEBUG_ALL:
        log_set_lvl(LOG_DEBUG);
        break;
    default:
        break;
    }
}

static uintptr_t _read_stack_ptr(void) {
    uintptr_t sp = 0;

#if defined(__x86_64__)
    asm volatile("mov %%rsp, %0" : "=r"(sp));
#else
    asm volatile("mov %%esp, %0" : "=r"(sp));
#endif

    return sp;
}

void arch_init(void* boot_info) {
    boot_info_t* info = boot_info;

    init_serial(SERIAL_COM1, SERAIL_DEFAULT_LINE, SERIAL_DEFAULT_BAUD);
    log_init(serial_puts);
    asm volatile("cld");

    if (!info)
        panic("boot info missing");

    select_log_level(info);

#if defined(__x86_64__)
    log_info("apheleiaOS kernel (x86_64) booting");
#else
    log_info("apheleiaOS kernel (x86_32) booting");
#endif

    gdt_init();
    tss_init(read_stack_ptr());
    pic_init();
    idt_init();
    pmm_init(&info->memory_map);
    heap_init();
    init_malloc();
    acpi_init(info->acpi_root_ptr);
    pci_init();
    if (info->args.debug == DEBUG_ALL)
        dump_pci_devices();

#if defined(__x86_64__)
    log_info("apheleiaOS kernel (x86_64) booted");
#else
    log_info("apheleiaOS kernel (x86_32) booted");
#endif
}

void arch_storage_init(void) {
    ata_disk_init();
}

u32 arch_pci_read(u8 bus, u8 slot, u8 func, u8 offset, u8 size) {
    const u16 pci_addr = 0xcf8;
    const u16 pci_data = 0xcfc;

    u32 addr =
        0x80000000 | ((u32)bus << 16) | ((u32)slot << 11) | ((u32)func << 8) | ((u32)offset & 0xfc);
    outl(pci_addr, addr);

    switch (size) {
    case 4:
        return inl(pci_data);
    case 2:
        return inw((u16)(pci_data + (offset & 2)));
    case 1:
        return inb((u16)(pci_data + (offset & 3)));
    default:
        return 0xffffffffU;
    }
}
