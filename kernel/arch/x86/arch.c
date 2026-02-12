#include <arch/arch.h>
#include <log/log.h>
#include <string.h>
#include <arch/thread.h>
#include <sys/acpi.h>
#include <sys/cpu.h>
#include <sys/font.h>
#include <sys/framebuffer.h>
#include <sys/panic.h>
#include <sys/pci.h>
#include <x86/asm.h>
#include <x86/ahci.h>
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

static void _route_irqs_to_pic(void) {
    outb(0x22, 0x70);
    u8 imcr = inb(0x23);
    outb(0x23, (u8)(imcr & ~0x01));
}

static void _cache_font(const boot_info_t* info) {
    if (!info) {
        font_path[0] = '\0';
        return;
    }

    strncpy(font_path, info->args.font, sizeof(font_path) - 1);
    font_path[sizeof(font_path) - 1] = '\0';
}

static bool _handle_user_signal(int signum, int_state_t* state) {
    if (!state)
        return false;

    if ((state->s_regs.cs & 0x3) != 3)
        return false;

    if (!sched_is_running())
        return false;

    sched_thread_t* thread = sched_current();
    if (!thread || !thread->user_thread)
        return false;

    sched_signal_send_thread(thread, signum);
    sched_signal_deliver_current(state);
    return true;
}

#define PF_ERR_PRESENT (1U << 0)
#define PF_ERR_WRITE   (1U << 1)
#define PF_ERR_USER    (1U << 2)

static void _page_fault_handler(int_state_t* state) {
    u64 addr = read_cr2();
    u64 code = state ? (u64)state->error_code : 0;
    u64 cr3 = read_cr3();

    bool present = (code & PF_ERR_PRESENT) != 0;
    bool write = (code & PF_ERR_WRITE) != 0;
    bool user = (code & PF_ERR_USER) != 0;

    if (present && write) {
        sched_thread_t* thread = sched_current();
        if (thread && thread->user_thread) {
            arch_word_t user_top = arch_user_stack_top();
            if (user || (user_top && addr < (u64)user_top)) {
                if (sched_handle_cow_fault(thread, (uintptr_t)addr, true))
                    return;
            }
        }
    }

    if (_handle_user_signal(SIGSEGV, state))
        return;

#if defined(__x86_64__)
    if (state) {
        log_fatal(
            "page fault: addr=%#llx err=%#llx cr3=%#llx rip=%#llx rsp=%#llx cs=%#llx",
            (unsigned long long)addr,
            (unsigned long long)code,
            (unsigned long long)cr3,
            (unsigned long long)state->s_regs.rip,
            (unsigned long long)state->s_regs.rsp,
            (unsigned long long)state->s_regs.cs
        );
    } else {
        log_fatal(
            "page fault: addr=%#llx err=%#llx cr3=%#llx",
            (unsigned long long)addr,
            (unsigned long long)code,
            (unsigned long long)cr3
        );
    }
#else
    if (state) {
        log_fatal(
            "page fault: addr=%#llx err=%#llx cr3=%#llx eip=%#llx esp=%#llx cs=%#llx",
            (unsigned long long)addr,
            (unsigned long long)code,
            (unsigned long long)cr3,
            (unsigned long long)state->s_regs.eip,
            (unsigned long long)state->s_regs.esp,
            (unsigned long long)state->s_regs.cs
        );
    } else {
        log_fatal(
            "page fault: addr=%#llx err=%#llx cr3=%#llx",
            (unsigned long long)addr,
            (unsigned long long)code,
            (unsigned long long)cr3
        );
    }
#endif
    disable_interrupts();
    halt();
}

static void _gp_fault_handler(int_state_t* state) {
    u64 code = state ? (u64)state->error_code : 0;

#if defined(__x86_64__)
    if (state) {
        log_fatal(
            "general protection fault: err=%#llx rip=%#llx cs=%#llx",
            (unsigned long long)code,
            (unsigned long long)state->s_regs.rip,
            (unsigned long long)state->s_regs.cs
        );
    } else {
        log_fatal("general protection fault: err=%#llx", (unsigned long long)code);
    }
#else
    if (state) {
        log_fatal(
            "general protection fault: err=%#llx eip=%#llx cs=%#llx",
            (unsigned long long)code,
            (unsigned long long)state->s_regs.eip,
            (unsigned long long)state->s_regs.cs
        );
    } else {
        log_fatal("general protection fault: err=%#llx", (unsigned long long)code);
    }
#endif

    disable_interrupts();
    halt();
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
    _route_irqs_to_pic();
    gdt_init();
    tss_init(read_stack_ptr());
    cpu_init_boot();
    pic_init();
    idt_init();
    set_int_handler(INT_PAGE_FAULT, _page_fault_handler);
    set_int_handler(INT_GENERAL_PROTECTION_FAULT, _gp_fault_handler);
    pmm_init(&info->memory_map);
    heap_init();
    init_malloc();
    pmm_ref_init();

    console_init(info);
    arch_publish_framebuffer(info);
    log_init(console_puts);

    acpi_init(info->acpi_root_ptr);
    tsc_init();
    irq_init();
    ps2_init();
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
    ahci_disk_init();
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

void arch_tlb_flush(uintptr_t addr) {
#if defined(__x86_64__)
    tlb_flush((u64)addr);
#else
    tlb_flush((u32)addr);
#endif
}

bool arch_pci_ecam_addr_ok(u64 addr) {
#if defined(__i386__)
    return addr < 0x100000000ULL;
#else
    return true;
#endif
}

void arch_cpu_set_local(void* ptr) {
#if defined(__x86_64__)
    if (ptr)
        set_gs_base((u64)(uintptr_t)ptr);
#else
    (void)ptr;
#endif
}

unsigned long arch_irq_save(void) {
    return irq_save();
}

void arch_irq_restore(unsigned long flags) {
    irq_restore(flags);
}

void arch_cpu_halt(void) {
    halt();
}

void arch_cpu_wait(void) {
    asm volatile("hlt");
}

void arch_irq_disable(void) {
    disable_interrupts();
}

u64 arch_timer_ticks(void) {
    return irq_ticks();
}

u32 arch_timer_hz(void) {
    return irq_timer_hz();
}

void arch_syscall_install(int vector, arch_syscall_handler_t handler) {
    set_int_handler(vector, handler);
    configure_int(vector, GDT_KERNEL_CODE, 0, IDT_TRP);
}

void arch_set_kernel_stack(uintptr_t sp) {
    set_tss_stack(sp);
}
