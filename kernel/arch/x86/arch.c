#include <arch/arch.h>
#include <log/log.h>
#include <string.h>
#include <sys/acpi.h>
#include <sys/font.h>
#include <sys/framebuffer.h>
#include <sys/panic.h>
#include <sys/pci.h>
#include <x86/asm.h>
#include <x86/ata.h>
#include <x86/boot.h>
#include <x86/console.h>
#include <x86/gdt.h>
#include <x86/idt.h>
#include <x86/mm/heap.h>
#include <x86/mm/physical.h>
#include <x86/pic.h>
#include <x86/serial.h>

static void _serial_puts(const char* s) {
    send_serial_string(SERIAL_COM1, s);
}

static void _console_puts(const char* s) {
    if (!s)
        return;

    arch_console_write(s, strlen(s));
}

static char font_path[128] = {0};

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

static void _cache_font(const boot_info_t* info) {
    if (!info) {
        font_path[0] = '\0';
        return;
    }

    strncpy(font_path, info->args.font, sizeof(font_path) - 1);
    font_path[sizeof(font_path) - 1] = '\0';
}

static void _publish_framebuffer(const boot_info_t* info) {
    framebuffer_info_t fb = {0};

    if (!info || info->video.mode != VIDEO_GRAPHICS || !info->video.framebuffer) {
        framebuffer_set_info(NULL);
        return;
    }

    u32 pitch = info->video.bytes_per_line;
    if (!pitch)
        pitch = info->video.width * info->video.bytes_per_pixel;

    fb.paddr = info->video.framebuffer;
    fb.width = info->video.width;
    fb.height = info->video.height;
    fb.pitch = pitch;
    fb.bpp = (u8)(info->video.bytes_per_pixel * 8);
    fb.red_mask = info->video.red_mask;
    fb.green_mask = info->video.green_mask;
    fb.blue_mask = info->video.blue_mask;
    fb.size = (u64)pitch * (u64)info->video.height;
    fb.available = fb.size != 0;

    framebuffer_set_info(&fb);
}

const char* arch_font_path(void) {
    if (!font_path[0])
        return NULL;

    return font_path;
}

void arch_set_font(const font_t* font) {
    console_set_font(font);
}

void arch_init(void* boot_info) {
    boot_info_t* info = boot_info;

    init_serial(SERIAL_COM1, SERAIL_DEFAULT_LINE, SERIAL_DEFAULT_BAUD);
    log_init(serial_puts);
    asm volatile("cld");

    if (!info)
        panic("boot info missing");

    _select_log_level(info);
    _cache_font(info);

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
    console_init(info);
    _publish_framebuffer(info);
    log_init(console_puts);

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
