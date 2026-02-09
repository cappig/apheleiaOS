#include <arch/arch.h>
#include <log/log.h>
#include <string.h>
#include <sys/acpi.h>
#include <sys/cpu.h>
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
#include <x86/irq.h>
#include <x86/mm/heap.h>
#include <x86/mm/physical.h>
#include <x86/pic.h>
#include <x86/ps2.h>
#include <x86/serial.h>
#include <x86/tsc.h>

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

static void _disable_apic_if_needed(void) {
#if defined(__i386__)
    u32 eflags = 0;
    u32 eflags_toggled = 0;

    asm volatile("pushfl\n\t"
                 "popl %0\n\t"
                 : "=r"(eflags));

    eflags_toggled = eflags ^ (1u << 21);

    asm volatile("pushl %0\n\t"
                 "popfl\n\t"
                 :
                 : "r"(eflags_toggled));

    asm volatile("pushfl\n\t"
                 "popl %0\n\t"
                 : "=r"(eflags_toggled));

    bool has_cpuid = ((eflags ^ eflags_toggled) & (1u << 21)) != 0;

    if (has_cpuid) {
        cpuid_regs_t regs = {0};
        cpuid(1, &regs);

        bool has_msr = (regs.edx & (1u << 5)) != 0;
        bool has_apic = (regs.edx & (1u << 9)) != 0;

        log_debug("apic: cpuid=%u msr=%u apic=%u", (u32)has_cpuid, (u32)has_msr, (u32)has_apic);

        if (has_msr && has_apic) {
            u64 apic_base = read_msr(0x1B);
            if (apic_base & (1ULL << 11)) {
                log_debug("apic: disabling local apic");
                write_msr(0x1B, apic_base & ~(1ULL << 11));
            } else {
                log_debug("apic: local apic already disabled");
            }
        }
    }

    // Route external interrupts through the PIC (IMCR).
    outb(0x22, 0x70);
    u8 imcr = inb(0x23);
    outb(0x23, (u8)(imcr | 0x01));
#endif
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

    _disable_apic_if_needed();
    gdt_init();
    tss_init(read_stack_ptr());
    cpu_init_boot();
    pic_init();
    idt_init();
    pmm_init(&info->memory_map);
    heap_init();
    init_malloc();
    tsc_init();
    irq_init();
    ps2_init();
    acpi_init(info->acpi_root_ptr);
    pci_init();
    if (info->args.debug == DEBUG_ALL)
        dump_pci_devices();
    console_init(info);
    _publish_framebuffer(info);
    log_init(console_puts);
    enable_interrupts();

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
