#include <arch/arch.h>
#include <arch/paging.h>
#include <arch/thread.h>
#include <base/macros.h>
#include <inttypes.h>
#include <libc_ext/string.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/acpi.h>
#include <sys/console.h>
#include <sys/cpu.h>
#include <sys/disk.h>
#include <sys/framebuffer.h>
#include <sys/logsink.h>
#include <sys/panic.h>
#include <sys/pci.h>
#include <sys/tty.h>
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

#define LOG_BOOT_HISTORY_CAP    (64 * 512)
#define BOOT_ROOTFS_SECTOR_SIZE 512

static bool log_console_ready = false;
static kernel_args_t boot_args = {0};
static u64 boot_rootfs_paddr = 0;
static size_t boot_rootfs_size = 0;
static bool boot_rootfs_registered = false;

static char boot_log_history[LOG_BOOT_HISTORY_CAP];
static size_t boot_log_history_len = 0;

typedef struct {
    u64 paddr;
    size_t size;
} boot_rootfs_t;


static void _log_history_append(const char *s, size_t len) {
    if (!s || !len || !LOG_BOOT_HISTORY_CAP) {
        return;
    }

    if (len >= LOG_BOOT_HISTORY_CAP) {
        memcpy(boot_log_history, s + (len - LOG_BOOT_HISTORY_CAP), LOG_BOOT_HISTORY_CAP);
        boot_log_history_len = LOG_BOOT_HISTORY_CAP;
        return;
    }

    if (boot_log_history_len + len > LOG_BOOT_HISTORY_CAP) {
        size_t drop = (boot_log_history_len + len) - LOG_BOOT_HISTORY_CAP;

        memmove(boot_log_history, boot_log_history + drop, boot_log_history_len - drop);
        boot_log_history_len -= drop;
    }

    memcpy(boot_log_history + boot_log_history_len, s, len);
    boot_log_history_len += len;
}

static void _log_history_replay_console(void) {
    if (!log_console_ready || !boot_log_history_len) {
        return;
    }

    console_write_screen(TTY_CONSOLE, boot_log_history, boot_log_history_len);
}

static void _log_write_early(const char *s, size_t len) {
    if (!s || !len) {
        return;
    }

    send_serial_sized_string(SERIAL_COM1, s, len);

    if (log_console_ready) {
        console_write_screen(TTY_CONSOLE, s, len);
    }
}

static void _log_puts(const char *s) {
    if (!s) {
        return;
    }

    size_t len = strlen(s);
    if (!len) {
        return;
    }

    _log_history_append(s, len);

    if (!logsink_is_bound()) {
        _log_write_early(s, len);
        return;
    }

    logsink_write(s, len);
}

void arch_panic_enter(void) {
    logsink_unbind_devices();
    log_console_ready = true;
    console_panic();
}

bool arch_supports_nx(void) {
#if defined(__x86_64__)
    static bool checked = false;
    static bool has_nx = false;

    if (!checked) {
        cpuid_regs_t regs = {0};
        cpuid(0x80000000, &regs);

        if (regs.eax >= CPUID_EXTENDED_INFO) {
            cpuid(CPUID_EXTENDED_INFO, &regs);

            if (regs.edx & CPUID_EI_NX) {
                u64 efer = read_msr(EFER_MSR);

                if (!(efer & EFER_NX)) {
                    write_msr(EFER_MSR, efer | EFER_NX);
                    efer = read_msr(EFER_MSR);
                }

                has_nx = (efer & EFER_NX) != 0;
            }
        }

        checked = true;
    }

    return has_nx;
#else
    return false;
#endif
}


void arch_fpu_init(void *buf) {
    if (!buf) {
        return;
    }

    asm volatile("fninit");
    asm volatile("fnsave %0" : "=m"(*(u8 *)buf));
    asm volatile("fninit");
}

void arch_fpu_save(void *buf) {
    if (!buf) {
        return;
    }

    asm volatile("fnsave %0" : "=m"(*(u8 *)buf));
}

void arch_fpu_restore(const void *buf) {
    if (!buf) {
        return;
    }

    asm volatile("frstor %0" : : "m"(*(const u8 *)buf));
}

static char cpu_name[64] = "x86";

static void _trim_cpu_name(char *name) {
    if (!name || !name[0]) {
        return;
    }

    char *trimmed = strtrim(name);
    strtrunc(trimmed);

    if (trimmed != name) {
        memmove(name, trimmed, strlen(trimmed) + 1);
    }
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

        if (cpu_name[0]) {
            return;
        }
    }

    cpuid(0x00000000, &regs);
    u32 vendor[3] = {regs.ebx, regs.edx, regs.ecx};
    memcpy(cpu_name, vendor, sizeof(vendor));

    cpu_name[sizeof(vendor)] = '\0';
}

static void _select_log_level(const boot_info_t *info) {
    if (!info) {
        return;
    }

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

static void _configure_log_sinks(const boot_info_t *info) {
    if (!info) {
        return;
    }

    logsink_reset();

    if (!info->args.console[0]) {
        logsink_add_target("/dev/console");
        return;
    }

    char devices[sizeof(info->args.console)];
    strncpy(devices, info->args.console, sizeof(devices) - 1);
    devices[sizeof(devices) - 1] = '\0';

    char *cursor = devices;

    while (cursor && *cursor) {
        char *next = strchr(cursor, ',');
        if (next) {
            *next = '\0';
        }

        char *token = strtrim(cursor);
        strtrunc(token);

        if (token[0]) {
            logsink_add_target(token);
        }

        if (!next) {
            break;
        }

        cursor = next + 1;
    }

    if (!logsink_has_targets()) {
        logsink_add_target("/dev/console");
    }
}

static bool _handle_user_signal(int signum, int_state_t *state) {
    if (!state) {
        return false;
    }

    if ((state->s_regs.cs & 0x3) != 3) {
        return false;
    }

    if (!sched_is_running()) {
        return false;
    }

    sched_thread_t *thread = sched_current();
    if (!thread || !thread->user_thread) {
        return false;
    }

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

static void _page_fault_handler(int_state_t *state) {
    u64 addr = read_cr2();
    u64 code = state ? (u64)state->error_code : 0;
    u64 cr3 = read_cr3();

    bool present = code & PF_ERR_PRESENT;
    bool write = code & PF_ERR_WRITE;
    bool user = code & PF_ERR_USER;

    if (present && write) {
        sched_thread_t *thread = sched_current();

        if (thread && thread->user_thread && _page_fault_is_user_addr(addr, user) &&
            sched_handle_cow_fault(thread, (uintptr_t)addr, true)) {
            return;
        }
    }

    if (_handle_user_signal(SIGSEGV, state)) {
        return;
    }

    panic_prepare();

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

static void _gp_fault_handler(int_state_t *state) {
    u64 code = state ? (u64)state->error_code : 0;

    if (_handle_user_signal(SIGSEGV, state)) {
        return;
    }

    panic_prepare();

#if defined(__x86_64__)
    if (state) {
        u64 rip = state->s_regs.rip;
        u64 cs = state->s_regs.cs;

        log_fatal(
            "general protection fault: err=%#" PRIx64 " rip=%#" PRIx64 " cs=%#" PRIx64,
            code,
            rip,
            cs
        );
    } else {
        log_fatal("general protection fault: err=%#" PRIx64, code);
    }
#else
    if (state) {
        u64 eip = state->s_regs.eip;
        u64 cs = state->s_regs.cs;

        log_fatal(
            "general protection fault: err=%#" PRIx64 " eip=%#" PRIx64 " cs=%#" PRIx64,
            code,
            eip,
            cs
        );
    } else {
        log_fatal("general protection fault: err=%#" PRIx64, code);
    }
#endif

    disable_interrupts();
    halt();
}

static void _double_fault_handler(UNUSED int_state_t *state) {
    panic_prepare();
    log_fatal("double fault (unrecoverable)");
    disable_interrupts();
    halt();
}

static void _invalid_opcode_handler(int_state_t *state) {
#if defined(__x86_64__)
    if (_handle_user_signal(SIGILL, state)) {
        return;
    }
#else
    if (_handle_user_signal(SIGILL, state))
        return;
#endif

    panic_prepare();

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

static void _publish_framebuffer(const boot_info_t *info) {
    framebuffer_info_t fb = {0};

    if (!info || info->video.mode != VIDEO_GRAPHICS || !info->video.framebuffer) {
        framebuffer_set_info(NULL);
        return;
    }

    u32 pitch = info->video.bytes_per_line;
    if (!pitch) {
        pitch = info->video.width * info->video.bytes_per_pixel;
    }

    fb.paddr = info->video.framebuffer;
    fb.width = info->video.width;
    fb.height = info->video.height;
    fb.pitch = pitch;
    fb.bpp = (u8)(info->video.bytes_per_pixel * 8);
    fb.red_shift = info->video.red_shift;
    fb.green_shift = info->video.green_shift;
    fb.blue_shift = info->video.blue_shift;
    fb.red_size = info->video.red_size;
    fb.green_size = info->video.green_size;
    fb.blue_size = info->video.blue_size;
    fb.size = (u64)pitch * (u64)info->video.height;
    fb.available = fb.size != 0;

    log_info(
        "framebuffer %ux%u bpp=%u pitch=%u rgb(r:%u/%u g:%u/%u b:%u/%u)",
        fb.width,
        fb.height,
        fb.bpp,
        fb.pitch,
        fb.red_shift,
        fb.red_size,
        fb.green_shift,
        fb.green_size,
        fb.blue_shift,
        fb.blue_size
    );

    framebuffer_set_info(&fb);
}

static ssize_t _boot_rootfs_read(disk_dev_t *dev, void *dest, size_t offset, size_t bytes) {
    if (!dev || !dest || !dev->private) {
        return -1;
    }

    boot_rootfs_t *rootfs = dev->private;

    if (offset >= rootfs->size) {
        return 0;
    }

    if (bytes > rootfs->size - offset) {
        bytes = rootfs->size - offset;
    }

    if (!bytes) {
        return 0;
    }

    void *src = arch_phys_map(rootfs->paddr + offset, bytes, 0);
    if (!src) {
        return -1;
    }

    memcpy(dest, src, bytes);
    arch_phys_unmap(src, bytes);

    return (ssize_t)bytes;
}

static ssize_t _boot_rootfs_write(disk_dev_t *dev, void *src, size_t offset, size_t bytes) {
    if (!dev || !src || !dev->private) {
        return -1;
    }

    boot_rootfs_t *rootfs = dev->private;

    if (offset >= rootfs->size) {
        return 0;
    }

    if (bytes > rootfs->size - offset) {
        bytes = rootfs->size - offset;
    }

    if (!bytes) {
        return 0;
    }

    void *dest = arch_phys_map(rootfs->paddr + offset, bytes, 0);
    if (!dest) {
        return -1;
    }

    memcpy(dest, src, bytes);
    arch_phys_unmap(dest, bytes);

    return (ssize_t)bytes;
}

static bool _register_boot_rootfs(void) {
    if (!boot_rootfs_paddr || !boot_rootfs_size || boot_rootfs_registered) {
        return false;
    }

    boot_rootfs_t *rootfs = calloc(1, sizeof(boot_rootfs_t));
    disk_dev_t *disk = calloc(1, sizeof(disk_dev_t));

    if (!rootfs || !disk) {
        free(rootfs);
        free(disk);
        return false;
    }

    rootfs->paddr = boot_rootfs_paddr;
    rootfs->size = boot_rootfs_size;

    static disk_interface_t interface = {
        .read = _boot_rootfs_read,
        .write = _boot_rootfs_write,
    };

    disk->name = strdup("ram0");
    disk->type = DISK_VIRTUAL;
    disk->sector_size = BOOT_ROOTFS_SECTOR_SIZE;
    disk->sector_count = DIV_ROUND_UP(rootfs->size, (size_t)BOOT_ROOTFS_SECTOR_SIZE);
    disk->interface = &interface;
    disk->private = rootfs;

    if (!disk->name || !disk->sector_count || !disk_register(disk)) {
        free(disk->name);
        free(disk);
        free(rootfs);
        return false;
    }

    boot_rootfs_registered = true;
    log_info("registered /dev/%s from boot image (%zu KiB)", disk->name, rootfs->size / 1024);

    return true;
}

const kernel_args_t *arch_init(void *boot_info) {
    boot_info_t *info = boot_info;

    init_serial(SERIAL_COM1, SERAIL_DEFAULT_LINE, SERIAL_DEFAULT_BAUD);
    asm volatile("cld");

    if (!info) {
        panic("boot info missing");
    }

    memcpy(&boot_args, &info->args, sizeof(boot_args));

    boot_rootfs_paddr = info->boot_rootfs_paddr;
    boot_rootfs_size = 0;

    if (info->boot_rootfs_size > (u64)(size_t)-1) {
        boot_rootfs_paddr = 0;
    } else {
        boot_rootfs_size = (size_t)info->boot_rootfs_size;
    }

    _configure_log_sinks(info);
    log_init(_log_puts);

    _select_log_level(info);
    _detect_cpu_name();

#if defined(__x86_64__)
    log_info("apheleiaOS kernel (x86_64) booting");
#else
    log_info("apheleiaOS kernel (x86_32) booting");
#endif

    if (boot_rootfs_paddr && boot_rootfs_size) {
        log_info(
            "staged rootfs at %#" PRIx64 " (%zu KiB)", boot_rootfs_paddr, boot_rootfs_size / 1024
        );
    }

    _route_irqs_to_pic();
    gdt_init();
    tss_init(_read_stack_ptr());
    cpu_init_boot();
    pat_init();
    pic_init();
    idt_init();

    set_int_handler(INT_PAGE_FAULT, _page_fault_handler);
    set_int_handler(INT_GENERAL_PROTECTION_FAULT, _gp_fault_handler);
    set_int_handler(INT_INVALID_OPCODE, _invalid_opcode_handler);
    set_int_handler(INT_DOUBLE_FAULT, _double_fault_handler);
#if defined(__x86_64__)
    configure_int(INT_DOUBLE_FAULT, GDT_KERNEL_CODE, 1, IDT_INT);
#endif

    pmm_init(&info->memory_map);
    heap_init();
    arch_init_alloc();
    pmm_ref_init();

    x86_console_backend_init();
    console_init(info);
    log_console_ready = true;
    _log_history_replay_console();
    _publish_framebuffer(info);

    acpi_init(info->acpi_root_ptr);
    tsc_init();
    irq_init();
    ps2_init();
    pci_init();
    if (info->args.debug == DEBUG_ALL) {
        dump_pci_devices();
    }
    console_init(info);
    _publish_framebuffer(info);
    enable_interrupts();

#if defined(__x86_64__)
    log_info("apheleiaOS kernel (x86_64) booted");
#else
    log_info("apheleiaOS kernel (x86_32) booted");
#endif

    return &boot_args;
}

void arch_storage_init(void) {
    ata_disk_init();
    ahci_disk_init();

    if (boot_rootfs_paddr && boot_rootfs_size && !_register_boot_rootfs()) {
        log_warn("failed to register staged rootfs fallback");
    }
}

void arch_register_devices(void) {
    serial_devfs_init();
}

void arch_tlb_flush(uintptr_t addr) {
#if defined(__x86_64__)
    tlb_flush((u64)addr);
#else
    tlb_flush((u32)addr);
#endif
}

void arch_cpu_set_local(void *ptr) {
#if defined(__x86_64__)
    if (ptr) {
        set_gs_base((u64)(uintptr_t)ptr);
    }
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

    if (seconds) {
        return seconds;
    }

    u64 hz = irq_timer_hz();
    if (!hz) {
        return 0;
    }

    return irq_ticks() / hz;
}

const char *arch_name(void) {
#if defined(__x86_64__)
    return "x86_64";
#else
    return "x86_32";
#endif
}

const char *arch_cpu_name(void) {
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
