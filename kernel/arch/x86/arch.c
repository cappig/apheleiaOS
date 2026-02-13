#include <arch/arch.h>
#include <arch/thread.h>
#include <inttypes.h>
#include <libc_ext/string.h>
#include <log/log.h>
#include <string.h>
#include <sys/acpi.h>
#include <sys/cpu.h>
#include <sys/font.h>
#include <sys/framebuffer.h>
#include <sys/panic.h>
#include <sys/pci.h>
#include <x86/ahci.h>
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
#include <x86/rtc.h>
#include <x86/serial.h>
#include <x86/tsc.h>

static void _serial_puts(const char* s) {
    send_serial_string(SERIAL_COM1, s);
}

typedef struct {
    bool console_enabled;
    bool serial_enabled;
    bool console_ready;
} log_sinks_t;

static log_sinks_t log_sinks = {
    .console_enabled = true,
    .serial_enabled = true,
    .console_ready = false,
};

static void _log_puts(const char* s) {
    if (!s)
        return;

    if (log_sinks.serial_enabled)
        serial_puts(s);

    if (log_sinks.console_enabled && log_sinks.console_ready)
        arch_console_write(s, strlen(s));
}

static char font_path[128] = {0};
static char cpu_name[64] = "x86";

static void _trim_cpu_name(char* name) {
    if (!name || !name[0])
        return;

    char* trimmed = strtrim(name);
    strtrunc(trimmed);

    if (trimmed != name)
        memmove(name, trimmed, strlen(trimmed) + 1);
}

static void _detect_cpu_name(void) {
    cpuid_regs_t regs = {0};
    cpuid(0x80000000, &regs);

    if (regs.eax >= 0x80000004) {
        u32 brand[12] = {0};
        cpuid_regs_t leaf = {0};

        cpuid(0x80000002, &leaf);
        brand[0] = leaf.eax;
        brand[1] = leaf.ebx;
        brand[2] = leaf.ecx;
        brand[3] = leaf.edx;

        cpuid(0x80000003, &leaf);
        brand[4] = leaf.eax;
        brand[5] = leaf.ebx;
        brand[6] = leaf.ecx;
        brand[7] = leaf.edx;

        cpuid(0x80000004, &leaf);
        brand[8] = leaf.eax;
        brand[9] = leaf.ebx;
        brand[10] = leaf.ecx;
        brand[11] = leaf.edx;

        memcpy(cpu_name, brand, sizeof(brand));
        cpu_name[sizeof(cpu_name) - 1] = '\0';
        _trim_cpu_name(cpu_name);

        if (cpu_name[0])
            return;
    }

    cpuid(0x00000000, &regs);
    u32 vendor[3] = {regs.ebx, regs.edx, regs.ecx};
    memcpy(cpu_name, vendor, sizeof(vendor));

    cpu_name[sizeof(vendor)] = '\0';
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

static bool _console_token_is_serial(const char* token) {
    if (!token || !token[0])
        return false;

    return !strcasecmp(token, "/dev/ttyS0") || !strcasecmp(token, "/dev/ttys0") ||
           !strcasecmp(token, "ttyS0") || !strcasecmp(token, "ttys0");
}

static bool _console_token_is_console(const char* token) {
    if (!token || !token[0])
        return false;

    return !strcasecmp(token, "/dev/console") || !strcasecmp(token, "console") ||
           !strcasecmp(token, "/dev/tty") || !strcasecmp(token, "tty") ||
           !strcasecmp(token, "/dev/tty0") || !strcasecmp(token, "tty0");
}

static void _configure_log_sinks(const boot_info_t* info) {
    if (!info)
        return;

    if (!info->args.console[0]) {
        log_sinks.console_enabled = true;
        log_sinks.serial_enabled = true;
        return;
    }

    char devices[sizeof(info->args.console)];
    strncpy(devices, info->args.console, sizeof(devices) - 1);
    devices[sizeof(devices) - 1] = '\0';

    bool console_enabled = false;
    bool serial_enabled = false;
    bool saw_valid_device = false;
    char* cursor = devices;

    while (cursor && *cursor) {
        char* next = strchr(cursor, ',');
        if (next)
            *next = '\0';

        char* token = strtrim(cursor);
        strtrunc(token);

        if (_console_token_is_console(token)) {
            console_enabled = true;
            saw_valid_device = true;
        } else if (_console_token_is_serial(token)) {
            serial_enabled = true;
            saw_valid_device = true;
        }

        if (!next)
            break;

        cursor = next + 1;
    }

    if (!saw_valid_device)
        console_enabled = true;

    log_sinks.console_enabled = console_enabled;
    log_sinks.serial_enabled = serial_enabled;
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

static bool _page_fault_is_user_addr(u64 addr, bool user) {
    arch_word_t user_top = arch_user_stack_top();
    return user || (user_top && addr < (u64)user_top);
}

static void _page_fault_handler(int_state_t* state) {
    u64 addr = read_cr2();
    u64 code = state ? (u64)state->error_code : 0;
    u64 cr3 = read_cr3();

    bool present = code & PF_ERR_PRESENT;
    bool write = code & PF_ERR_WRITE;
    bool user = code & PF_ERR_USER;

    if (present && write) {
        sched_thread_t* thread = sched_current();

        if (thread && thread->user_thread && _page_fault_is_user_addr(addr, user) &&
            sched_handle_cow_fault(thread, (uintptr_t)addr, true))
            return;
    }

    if (_handle_user_signal(SIGSEGV, state))
        return;

#if defined(__x86_64__)
    if (state) {
        u64 rip = state->s_regs.rip;
        u64 rsp = state->s_regs.rsp;
        u64 cs = state->s_regs.cs;

        log_fatal(
            "page fault: addr=%#" PRIx64 " err=%#" PRIx64 " cr3=%#" PRIx64 " rip=%#" PRIx64
            " rsp=%#" PRIx64 " cs=%#" PRIx64,
            addr,
            code,
            cr3,
            rip,
            rsp,
            cs
        );
    } else {
        log_fatal("page fault: addr=%#" PRIx64 " err=%#" PRIx64 " cr3=%#" PRIx64, addr, code, cr3);
    }
#else
    if (state) {
        u64 eip = state->s_regs.eip;
        u64 esp = state->s_regs.esp;
        u64 cs = state->s_regs.cs;

        log_fatal(
            "page fault: addr=%#" PRIx64 " err=%#" PRIx64 " cr3=%#" PRIx64 " eip=%#" PRIx64
            " esp=%#" PRIx64 " cs=%#" PRIx64,
            addr,
            code,
            cr3,
            eip,
            esp,
            cs
        );
    } else {
        log_fatal("page fault: addr=%#" PRIx64 " err=%#" PRIx64 " cr3=%#" PRIx64, addr, code, cr3);
    }
#endif

    disable_interrupts();
    halt();
}

static void _gp_fault_handler(int_state_t* state) {
    u64 code = state ? (u64)state->error_code : 0;

#if defined(__x86_64__)
    if (state) {
        u64 rip = state->s_regs.rip;
        u64 cs = state->s_regs.cs;

        log_fatal(
            "general protection fault: err=%#" PRIx64 " rip=%#" PRIx64 " cs=%#" PRIx64, code, rip, cs
        );
    } else {
        log_fatal("general protection fault: err=%#" PRIx64, code);
    }
#else
    if (state) {
        u64 eip = state->s_regs.eip;
        u64 cs = state->s_regs.cs;

        log_fatal(
            "general protection fault: err=%#" PRIx64 " eip=%#" PRIx64 " cs=%#" PRIx64, code, eip, cs
        );
    } else {
        log_fatal("general protection fault: err=%#" PRIx64, code);
    }
#endif

    disable_interrupts();
    halt();
}

static void _invalid_opcode_handler(int_state_t* state) {
#if defined(__x86_64__)
    if (_handle_user_signal(SIGILL, state))
        return;
#else
    if (_handle_user_signal(SIGILL, state))
        return;
#endif

#if defined(__x86_64__)
    if (state) {
        u64 rip = state->s_regs.rip;
        u64 cs = state->s_regs.cs;

        log_fatal("invalid opcode: rip=%#" PRIx64 " cs=%#" PRIx64, rip, cs);
    } else {
        log_fatal("invalid opcode");
    }
#else
    if (state) {
        u64 eip = state->s_regs.eip;
        u64 cs = state->s_regs.cs;

        log_fatal("invalid opcode: eip=%#" PRIx64 " cs=%#" PRIx64, eip, cs);
    } else {
        log_fatal("invalid opcode");
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
    asm volatile("cld");

    if (!info)
        panic("boot info missing");

    _configure_log_sinks(info);
    log_init(_log_puts);

    select_log_level(info);
    _cache_font(info);
    _detect_cpu_name();

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
    set_int_handler(INT_GENERAL_PROTECTION_FAULT, gp_fault_handler);
    set_int_handler(INT_INVALID_OPCODE, _invalid_opcode_handler);

    pmm_init(&info->memory_map);
    heap_init();
    arch_init_alloc();
    pmm_ref_init();

    console_init(info);
    log_sinks.console_ready = true;
    publish_framebuffer(info);

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

void arch_tlb_flush(uintptr_t addr) {
#if defined(__x86_64__)
    tlb_flush((u64)addr);
#else
    tlb_flush((u32)addr);
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

u64 arch_wallclock_seconds(void) {
    u64 seconds = x86_rtc_unix_seconds();

    if (seconds)
        return seconds;

    u64 hz = irq_timer_hz();
    if (!hz)
        return 0;

    return irq_ticks() / hz;
}

const char* arch_name(void) {
#if defined(__x86_64__)
    return "x86_64";
#else
    return "x86_32";
#endif
}

const char* arch_cpu_name(void) {
    return cpu_name;
}

u64 arch_cpu_khz(void) {
    return tsc_khz();
}

size_t arch_mem_total(void) {
    return pmm_total_mem();
}

size_t arch_mem_free(void) {
    return pmm_free_mem();
}

void arch_syscall_install(int vector, arch_syscall_handler_t handler) {
    set_int_handler(vector, handler);
    configure_int(vector, GDT_KERNEL_CODE, 0, IDT_TRP);
}

void arch_set_kernel_stack(uintptr_t sp) {
    set_tss_stack(sp);
}
