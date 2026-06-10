#include <arch/arch.h>
#include <arch/paging.h>
#include <arch/thread.h>
#include <base/macros.h>
#include <drivers/manager.h>
#include <fs/ext2.h>
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
#include <sys/keyboard.h>
#include <sys/lock.h>
#include <sys/logsink.h>
#include <sys/mouse.h>
#include <sys/panic.h>
#include <sys/pci.h>
#include <sys/tty.h>
#include <x86/apic.h>
#include <x86/asm.h>
#include <x86/boot.h>
#include <x86/console.h>
#include <x86/gdt.h>
#include <x86/idt.h>
#include <x86/irq.h>
#include <x86/mm/heap.h>
#include <x86/mm/physical.h>
#include <x86/pic.h>
#include <x86/rtc.h>
#include <x86/serial.h>
#include <x86/smp.h>
#include <x86/tsc.h>

#define LOG_BOOT_HISTORY_CAP     (256 * 1024)
#define BOOT_ROOTFS_SECTOR_SIZE  512
#define CPUID_FEATURES           0x00000001
#define CPUID_FEAT_EDX_FXSR      (1U << 24)
#define CPUID_FEAT_EDX_SSE       (1U << 25)
#define WALLCLOCK_RTC_RESYNC_SEC 60ULL

typedef struct {
    bool console_ready;
    char history[LOG_BOOT_HISTORY_CAP];
    size_t history_len;
    spinlock_t lock;
} x86_log_state_t;

typedef struct {
    kernel_args_t args;
    u64 rootfs_paddr;
    size_t rootfs_size;
    bool rootfs_registered;
    boot_root_hint_t root_hint;
    volatile u32 wallclock_sync_inflight;
} x86_boot_state_t;

typedef struct {
    bool fxsr;
    char name[64];
} x86_cpu_state_t;

static x86_log_state_t x86_log = {
    .lock = SPINLOCK_INIT,
};

static x86_boot_state_t x86_boot = { 0 };

static x86_cpu_state_t x86_cpu = {
    .name = "x86",
};

static volatile u64 wallclock_base_seconds ALIGNED(8) = 0;
static volatile u64 wallclock_base_ticks ALIGNED(8) = 0;
static volatile u64 rtc_sync_ticks ALIGNED(8) = 0;

#if defined(__x86_64__)
static bool _cpu_ptr_is_core_local(const cpu_core_t *core) {
    uintptr_t ptr = (uintptr_t)core;
    uintptr_t first = (uintptr_t)&cores_local[0];
    uintptr_t last = (uintptr_t)&cores_local[MAX_CORES];

    if (ptr < first || ptr >= last) {
        return false;
    }

    return ((ptr - first) % sizeof(cpu_core_t)) == 0;
}
#endif

typedef struct {
    u64 paddr;
    size_t size;
} boot_rootfs_t;

#if defined(__i386__)
#define FX_HELPER_ATTR __attribute__((noinline, force_align_arg_pointer))
#else
#define FX_HELPER_ATTR __attribute__((noinline))
#endif

static void FX_HELPER_ATTR _fxsave_unaligned(void *buf) {
    u8 bounce[512] __attribute__((aligned(16)));
    asm volatile("fxsave %0" : "=m"(*(u8(*)[512])bounce));
    memcpy(buf, bounce, sizeof(bounce));
}

static void FX_HELPER_ATTR _fxrstor_unaligned(const void *buf) {
    u8 bounce[512] __attribute__((aligned(16)));
    memcpy(bounce, buf, sizeof(bounce));
    asm volatile("fxrstor %0" : : "m"(*(const u8(*)[512])bounce));
}

static void _log_history_append(const char *s, size_t len) {
    if (!s || !len || !LOG_BOOT_HISTORY_CAP) {
        return;
    }

    if (len >= LOG_BOOT_HISTORY_CAP) {
        memcpy(x86_log.history, s + (len - LOG_BOOT_HISTORY_CAP), LOG_BOOT_HISTORY_CAP);
        x86_log.history_len = LOG_BOOT_HISTORY_CAP;
        return;
    }

    if (x86_log.history_len + len > LOG_BOOT_HISTORY_CAP) {
        size_t drop = (x86_log.history_len + len) - LOG_BOOT_HISTORY_CAP;

        memmove(x86_log.history, x86_log.history + drop, x86_log.history_len - drop);
        x86_log.history_len -= drop;
    }

    memcpy(x86_log.history + x86_log.history_len, s, len);
    x86_log.history_len += len;
}

static const char *_boot_log_ptr(u64 paddr) {
#if defined(__x86_64__)
    return (const char *)(uintptr_t)(paddr + LINEAR_MAP_OFFSET_64);
#else
    return (const char *)(uintptr_t)paddr;
#endif
}

static void _append_bootloader_log(const boot_info_t *info) {
    if (!info || !info->boot_log_paddr || !info->boot_log_len) {
        return;
    }

    size_t len = info->boot_log_len;
    if (info->boot_log_cap && len > info->boot_log_cap) {
        len = info->boot_log_cap;
    }

    if (!len) {
        return;
    }

    const char *buf = _boot_log_ptr(info->boot_log_paddr);
    if (!buf) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&x86_log.lock);
    _log_history_append(buf, len);
    spin_unlock_irqrestore(&x86_log.lock, flags);
}

static bool _e820_range_reserved(const e820_map_t *map, u64 start, size_t size) {
    if (!map || !start || !size || start >= PROTECTED_MODE_TOP) {
        return false;
    }

    u64 end = start + (u64)size;
    if (end <= start || end > PROTECTED_MODE_TOP) {
        return false;
    }

    size_t count = map->count < E820_MAX ? (size_t)map->count : E820_MAX;
    u64 cursor = start;

    while (cursor < end) {
        bool covered = false;

        for (size_t i = 0; i < count; i++) {
            const e820_entry_t *entry = &map->entries[i];
            u64 entry_end = entry->address + entry->size;

            if (entry->type == E820_AVAILABLE) {
                continue;
            }

            if (entry_end <= entry->address) {
                continue;
            }

            if (cursor < entry->address || cursor >= entry_end) {
                continue;
            }

            cursor = entry_end < end ? entry_end : end;
            covered = true;
            break;
        }

        if (!covered) {
            return false;
        }
    }

    return true;
}

static void _replay_boot_log(void) {
    if (!x86_log.console_ready || !x86_log.history_len) {
        return;
    }

    console_write_screen(TTY_CONSOLE, x86_log.history, x86_log.history_len);
}

void arch_log_replay_console(void) {
    if (!x86_log.console_ready) {
        return;
    }

    size_t snapshot_len = 0;
    unsigned long flags = spin_lock_irqsave(&x86_log.lock);
    snapshot_len = x86_log.history_len;
    spin_unlock_irqrestore(&x86_log.lock, flags);

    if (!snapshot_len) {
        return;
    }

    char chunk[512];
    size_t offset = 0;

    while (offset < snapshot_len) {
        size_t chunk_len = snapshot_len - offset;
        if (chunk_len > sizeof(chunk)) {
            chunk_len = sizeof(chunk);
        }

        ssize_t read_len = arch_log_ring_read(chunk, offset, chunk_len);
        if (read_len <= 0) {
            break;
        }

        console_write_screen(TTY_CONSOLE, chunk, (size_t)read_len);
        offset += (size_t)read_len;
    }
}

void arch_debug_write(const char *s, size_t len) {
    if (!s || !len) {
        return;
    }

    send_serial_buf(SERIAL_COM1, s, len);
}

ssize_t arch_log_ring_read(void *buf, size_t offset, size_t len) {
    if (!buf) {
        return -1;
    }

    if (!len) {
        return 0;
    }

    unsigned long irq_flags = spin_lock_irqsave(&x86_log.lock);
    size_t history_len = x86_log.history_len;

    if (offset >= history_len) {
        spin_unlock_irqrestore(&x86_log.lock, irq_flags);
        return 0;
    }

    size_t copy_len = history_len - offset;
    if (copy_len > len) {
        copy_len = len;
    }

    memcpy(buf, x86_log.history + offset, copy_len);
    spin_unlock_irqrestore(&x86_log.lock, irq_flags);
    return (ssize_t)copy_len;
}

size_t arch_log_ring_size(void) {
    unsigned long irq_flags = spin_lock_irqsave(&x86_log.lock);
    size_t history_len = x86_log.history_len;
    spin_unlock_irqrestore(&x86_log.lock, irq_flags);
    return history_len;
}

static void _log_write_early(const char *s, size_t len) {
    if (!s || !len) {
        return;
    }

    send_serial_buf(SERIAL_COM1, s, len);

    if (x86_log.console_ready) {
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

    unsigned long flags = spin_lock_irqsave(&x86_log.lock);
    _log_history_append(s, len);
    spin_unlock_irqrestore(&x86_log.lock, flags);

    if (!logsink_is_bound()) {
        _log_write_early(s, len);
        return;
    }

    logsink_write(s, len);
}

void arch_panic_enter(void) {
    logsink_unbind_devices();
    x86_log.console_ready = true;
    console_panic();
}

bool arch_supports_nx(void) {
#if defined(__x86_64__)
    static bool checked = false;
    static bool has_nx = false;

    if (!checked) {
        cpuid_regs_t regs = { 0 };
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

static inline void _fxsave_buf(void *buf) {
    if (!buf) {
        return;
    }

    if (((uintptr_t)buf & 0x0f) == 0) {
        asm volatile("fxsave %0" : "=m"(*(u8(*)[512])buf));
        return;
    }

    _fxsave_unaligned(buf);
}

static inline void _fxrstor_buf(const void *buf) {
    if (!buf) {
        return;
    }

    if (((uintptr_t)buf & 0x0f) == 0) {
        asm volatile("fxrstor %0" : : "m"(*(const u8(*)[512])buf));
        return;
    }

    _fxrstor_unaligned(buf);
}

void arch_fpu_init(void *buf) {
    if (!buf) {
        return;
    }

    if (x86_cpu.fxsr) {
        asm volatile("fninit");
        _fxsave_buf(buf);
        asm volatile("fninit");
        return;
    }

    asm volatile("fninit");
    asm volatile("fnsave %0" : "=m"(*(u8(*)[108])buf));
    asm volatile("fninit");
}

void arch_fpu_save(void *buf) {
    if (!buf) {
        return;
    }

    if (x86_cpu.fxsr) {
        _fxsave_buf(buf);
        return;
    }

    asm volatile("fnsave %0" : "=m"(*(u8(*)[108])buf));
}

void arch_fpu_restore(const void *buf) {
    if (!buf) {
        return;
    }

    if (x86_cpu.fxsr) {
        _fxrstor_buf(buf);
        return;
    }

    asm volatile("frstor %0" : : "m"(*(const u8(*)[108])buf));
}

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
    cpuid_regs_t regs = { 0 };
    cpuid(0x80000000, &regs);

    if (regs.eax >= 0x80000004) {
        u32 brand[12] = { 0 };
        cpuid_regs_t leaf = { 0 };

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

        memcpy(x86_cpu.name, brand, sizeof(brand));
        x86_cpu.name[sizeof(x86_cpu.name) - 1] = '\0';
        _trim_cpu_name(x86_cpu.name);

        if (x86_cpu.name[0]) {
            return;
        }
    }

    cpuid(0x00000000, &regs);
    u32 vendor[3] = { regs.ebx, regs.edx, regs.ecx };
    memcpy(x86_cpu.name, vendor, sizeof(vendor));

    x86_cpu.name[sizeof(vendor)] = '\0';
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

static bool _cpu_has_fxsr_sse(void) {
    cpuid_regs_t regs = { 0 };
    cpuid(CPUID_FEATURES, &regs);

    if (!(regs.edx & CPUID_FEAT_EDX_FXSR)) {
        return false;
    }

    if (!(regs.edx & CPUID_FEAT_EDX_SSE)) {
        return false;
    }

    return true;
}

static void _fpu_hw_enable(void) {
    u64 cr0 = read_cr0();
    cr0 &= ~(u64)(CR0_EM | CR0_TS);
    cr0 |= (u64)CR0_MP;
    write_cr0(cr0);

    x86_cpu.fxsr = _cpu_has_fxsr_sse();
    if (!x86_cpu.fxsr) {
#if defined(__i386__)
        panic("x86_32 build requires SSE/FXSR support");
#else
        return;
#endif
    }

    u64 cr4 = read_cr4();
    cr4 |= (u64)(CR4_OSFXSR | CR4_OSXMMEXCPT);
    write_cr4(cr4);

    asm volatile("fninit");
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

static bool _is_user_fault_addr(u64 addr, bool user) {
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

        bool is_user_addr = _is_user_fault_addr(addr, user);
        bool can_cow = thread && thread->user_thread && is_user_addr;

        bool cow_handled = false;
        if (can_cow) {
            cow_handled = sched_handle_cow_fault(thread, (uintptr_t)addr, true);
        }

        if (cow_handled) {
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
        u64 cs = state->s_regs.cs;
        bool user_frame = (cs & 0x3) == 3;

        if (user_frame) {
            log_fatal(
                "page fault addr=%#" PRIx64 " err=%#" PRIx64 " cr3=%#" PRIx64 " rip=%#" PRIx64 " rsp=%#" PRIx64
                " cs=%#" PRIx64,
                addr,
                code,
                cr3,
                rip,
                (u64)state->s_regs.rsp,
                cs
            );
        } else {
            log_fatal(
                "page fault addr=%#" PRIx64 " err=%#" PRIx64 " cr3=%#" PRIx64 " rip=%#" PRIx64 " ksp=%#" PRIx64
                " cs=%#" PRIx64,
                addr,
                code,
                cr3,
                rip,
                (u64)_read_stack_ptr(),
                cs
            );
        }
    } else {
        log_fatal("page fault addr=%#" PRIx64 " err=%#" PRIx64 " cr3=%#" PRIx64, addr, code, cr3);
    }
#else
    if (state) {
        u64 eip = state->s_regs.eip;
        u64 cs = state->s_regs.cs;
        bool user_frame = (cs & 0x3) == 3;

        if (user_frame) {
            log_fatal(
                "page fault addr=%#" PRIx64 " err=%#" PRIx64 " cr3=%#" PRIx64 " eip=%#" PRIx64 " esp=%#" PRIx64
                " cs=%#" PRIx64,
                addr,
                code,
                cr3,
                eip,
                (u64)state->s_regs.esp,
                cs
            );
        } else {
            log_fatal(
                "page fault addr=%#" PRIx64 " err=%#" PRIx64 " cr3=%#" PRIx64 " eip=%#" PRIx64 " ksp=%#" PRIx64
                " cs=%#" PRIx64,
                addr,
                code,
                cr3,
                eip,
                (u64)_read_stack_ptr(),
                cs
            );
        }
    } else {
        log_fatal("page fault addr=%#" PRIx64 " err=%#" PRIx64 " cr3=%#" PRIx64, addr, code, cr3);
    }
#endif

    panic_dump_state(state);

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

        log_fatal("general protection fault err=%#" PRIx64 " rip=%#" PRIx64 " cs=%#" PRIx64, code, rip, cs);
    } else {
        log_fatal("general protection fault err=%#" PRIx64, code);
    }
#else
    if (state) {
        u64 eip = state->s_regs.eip;
        u64 cs = state->s_regs.cs;

        log_fatal("general protection fault err=%#" PRIx64 " eip=%#" PRIx64 " cs=%#" PRIx64, code, eip, cs);
    } else {
        log_fatal("general protection fault err=%#" PRIx64, code);
    }
#endif

    panic_dump_state(state);

    disable_interrupts();
    halt();
}

static void _double_fault_handler(int_state_t *state) {
    panic_prepare();

    log_fatal("double fault (unrecoverable)");

    panic_dump_state(state);
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

    sched_thread_t *current = sched_current();
    if (current) {
#if defined(__x86_64__)
        u64 ctx_ip = 0;
        u64 ctx_cs = 0;
        uintptr_t stack_base = (uintptr_t)current->stack;
        uintptr_t stack_end = stack_base + current->stack_size;
        uintptr_t ctx_ptr = current->context;
        size_t kernel_frame_need = offsetof(arch_int_state_t, s_regs) + (3U * sizeof(arch_word_t));
        if (current->context && arch_kernel_stack_valid(current) && stack_end >= stack_base && ctx_ptr >= stack_base &&
            ctx_ptr < stack_end && (size_t)(stack_end - ctx_ptr) >= kernel_frame_need) {
            const arch_int_state_t *ctx = (const arch_int_state_t *)(uintptr_t)current->context;
            ctx_ip = ctx->s_regs.rip;
            ctx_cs = ctx->s_regs.cs;
        }
#else
        u64 ctx_ip = 0;
        u64 ctx_cs = 0;
        uintptr_t stack_base = (uintptr_t)current->stack;
        uintptr_t stack_end = stack_base + current->stack_size;
        uintptr_t ctx_ptr = current->context;
        size_t kernel_frame_need = offsetof(arch_int_state_t, s_regs) + (3U * sizeof(arch_word_t));
        if (current->context && arch_kernel_stack_valid(current) && stack_end >= stack_base && ctx_ptr >= stack_base &&
            ctx_ptr < stack_end && (size_t)(stack_end - ctx_ptr) >= kernel_frame_need) {
            const arch_int_state_t *ctx = (const arch_int_state_t *)(uintptr_t)current->context;
            ctx_ip = ctx->s_regs.eip;
            ctx_cs = ctx->s_regs.cs;
        }
#endif

        log_fatal(
            "current thread pid=%d name=%s context=%#" PRIx64 " stack=%#" PRIx64 " stack_size=%zu cpu=%d state=%d"
            " ctx_ip=%#" PRIx64 " ctx_cs=%#" PRIx64,
            current->pid,
            current->name,
            (u64)(uintptr_t)current->context,
            (u64)(uintptr_t)current->stack,
            current->stack_size,
            current->running_cpu,
            current->state,
            ctx_ip,
            ctx_cs
        );
    } else {
        log_fatal("current thread none");
    }

#if defined(__x86_64__)
    if (state) {
        u64 rip = state->s_regs.rip;
        u64 cs = state->s_regs.cs;

        log_fatal("invalid opcode rip=%#" PRIx64 " cs=%#" PRIx64, rip, cs);
    } else {
        log_fatal("invalid opcode");
    }
#else
    if (state) {
        u64 eip = state->s_regs.eip;
        u64 cs = state->s_regs.cs;

        log_fatal("invalid opcode eip=%#" PRIx64 " cs=%#" PRIx64, eip, cs);
    } else {
        log_fatal("invalid opcode");
    }
#endif

    panic_dump_state(state);
    disable_interrupts();
    halt();
}

static void _publish_framebuffer(const boot_info_t *info) {
    framebuffer_info_t fb = { 0 };

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
        (unsigned int)fb.width,
        (unsigned int)fb.height,
        (unsigned int)fb.bpp,
        (unsigned int)fb.pitch,
        (unsigned int)fb.red_shift,
        (unsigned int)fb.red_size,
        (unsigned int)fb.green_shift,
        (unsigned int)fb.green_size,
        (unsigned int)fb.blue_shift,
        (unsigned int)fb.blue_size
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
    if (!x86_boot.rootfs_paddr || !x86_boot.rootfs_size || x86_boot.rootfs_registered) {
        return false;
    }

    boot_rootfs_t *rootfs = calloc(1, sizeof(boot_rootfs_t));
    disk_dev_t *disk = calloc(1, sizeof(disk_dev_t));

    if (!rootfs || !disk) {
        free(rootfs);
        free(disk);
        return false;
    }

    rootfs->paddr = x86_boot.rootfs_paddr;
    rootfs->size = x86_boot.rootfs_size;

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

    x86_boot.rootfs_registered = true;
    log_info("registered /dev/%s from boot rootfs (%zu KiB)", disk->name, rootfs->size / 1024);

    return true;
}

static void set_boot_root_uuid(void) {
    if (x86_boot.root_hint.valid && x86_boot.root_hint.rootfs_uuid_valid) {
        disk_set_root_uuid(x86_boot.root_hint.rootfs_uuid);
        return;
    }

    if (!x86_boot.rootfs_paddr || !x86_boot.rootfs_size) {
        disk_clear_root_uuid();
        return;
    }

    if (x86_boot.rootfs_size < sizeof(ext2_superblock_t) + 1024) {
        disk_clear_root_uuid();
        return;
    }

    ext2_superblock_t sb = { 0 };
    void *map = arch_phys_map(x86_boot.rootfs_paddr + 1024, sizeof(sb), 0);

    if (!map) {
        disk_clear_root_uuid();
        return;
    }

    memcpy(&sb, map, sizeof(sb));
    arch_phys_unmap(map, sizeof(sb));

    if (sb.signature != EXT2_SIGNATURE) {
        disk_clear_root_uuid();
        return;
    }

    disk_set_root_uuid(sb.fs_id);
}

static void _set_boot_disk_hint(void) {
    if (!x86_boot.root_hint.valid) {
        disk_clear_boot_hint();
        return;
    }

    disk_boot_hint_t hint = {
        .valid = x86_boot.root_hint.valid != 0,
        .media = x86_boot.root_hint.media,
        .transport = x86_boot.root_hint.transport,
        .part_style = x86_boot.root_hint.part_style,
        .part_index = x86_boot.root_hint.part_index,
        .bios_drive = x86_boot.root_hint.bios_drive,
    };

    disk_set_boot_hint(&hint);
}

const kernel_args_t *arch_init(void *boot_info) {
    boot_info_t *info = boot_info;

    init_serial(SERIAL_COM1, SERIAL_DEFAULT_LINE, SERIAL_DEFAULT_BAUD);
    asm volatile("cld");

    if (!info) {
        panic("boot info missing");
    }

    memcpy(&x86_boot.args, &info->args, sizeof(x86_boot.args));
    smp_set_boot_info(info);
    _append_bootloader_log(info);

    x86_boot.rootfs_paddr = info->boot_rootfs_paddr;
    x86_boot.rootfs_size = 0;
    x86_boot.root_hint = info->boot_root_hint;

    if (info->boot_rootfs_size > (u64)(size_t)-1) {
        x86_boot.rootfs_paddr = 0;
    } else {
        x86_boot.rootfs_size = (size_t)info->boot_rootfs_size;
    }

    bool staged_rootfs = x86_boot.rootfs_paddr && x86_boot.rootfs_size;
    bool rootfs_reserved = false;

    if (staged_rootfs) {
        rootfs_reserved = _e820_range_reserved(&info->memory_map, x86_boot.rootfs_paddr, x86_boot.rootfs_size);
    }

    if (staged_rootfs && !rootfs_reserved) {
        panic("staged rootfs memory is allocatable");
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

    if (x86_boot.rootfs_paddr && x86_boot.rootfs_size) {
        log_info("boot rootfs at %#" PRIx64 " (%zu KiB)", x86_boot.rootfs_paddr, x86_boot.rootfs_size / 1024);
    }

    _route_irqs_to_pic();

    gdt_init();
    tss_init(_read_stack_ptr());

    cpu_init_boot();
    _fpu_hw_enable();

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
    if (!pmm_ref_ready()) {
        panic("PMM refcount table unavailable, COW fork is required");
    }

    x86_console_backend_init();
    console_init(info);
    x86_log.console_ready = true;

    _replay_boot_log();
    _publish_framebuffer(info);

    acpi_init(info->acpi_root_ptr);
    tsc_init();
    irq_init();

    pci_init();

    if (!keyboard_init()) {
        log_warn("keyboard core setup failed");
    }

    if (!mouse_init()) {
        log_warn("mouse core setup failed");
    }

    if (!driver_registry_init()) {
        log_warn("driver registry setup failed");
    } else {
        driver_load_stage(DRIVER_STAGE_ARCH_EARLY);
    }

    if (info->args.debug == DEBUG_ALL) {
        dump_pci_devices();
    }

    enable_interrupts();

#if defined(__x86_64__)
    log_info("apheleiaOS kernel (x86_64) booted");
#else
    log_info("apheleiaOS kernel (x86_32) booted");
#endif

    return &x86_boot.args;
}

void arch_smp_init(void) {
    smp_init();
}

void arch_storage_init(void) {
    _set_boot_disk_hint();
    set_boot_root_uuid();

    driver_load_stage(DRIVER_STAGE_STORAGE);

    if (x86_boot.rootfs_paddr && x86_boot.rootfs_size && !_register_boot_rootfs()) {
        log_warn("failed to register boot rootfs fallback");
    }
}

void arch_late_init(void) {
}

bool arch_phys_map_can_persist(void) {
#if defined(__x86_64__)
    return true;
#else
    return false;
#endif
}

void arch_tlb_flush(uintptr_t addr) {
#if defined(__x86_64__)
    tlb_flush((u64)addr);
#else
    tlb_flush((u32)addr);
#endif
    smp_tlb_shootdown(addr);
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

void *arch_cpu_get_local(void) {
#if defined(__x86_64__)
    u64 gs_base = read_msr(GS_BASE);
    cpu_core_t *core = (cpu_core_t *)(uintptr_t)gs_base;

    if (_cpu_ptr_is_core_local(core)) {
        return core;
    }

    u64 kernel_gs_base = read_msr(KERNEL_GS_BASE);
    core = (cpu_core_t *)(uintptr_t)kernel_gs_base;

    if (_cpu_ptr_is_core_local(core)) {
        return core;
    }

    cpuid_regs_t regs = { 0 };
    cpuid(1, &regs);
    u32 cpuid_apic_id = (regs.ebx >> 24) & 0xffU;
    u32 live_apic_id = lapic_id();

    if (live_apic_id || live_apic_id == cpuid_apic_id) {
        core = cpu_find_by_lapic(live_apic_id);
        if (core) {
            return core;
        }
    }

    return cpu_find_by_lapic(cpuid_apic_id);
#else
    size_t core_id = 0;
    if (gdt_current_core_id(&core_id) && core_id < MAX_CORES) {
        cpu_core_t *core = &cores_local[core_id];
        if (core->valid) {
            return core;
        }
    }

    cpuid_regs_t regs = { 0 };
    cpuid(1, &regs);
    u32 cpuid_apic_id = (regs.ebx >> 24) & 0xffU;
    u32 live_apic_id = lapic_id();

    if (live_apic_id || live_apic_id == cpuid_apic_id) {
        cpu_core_t *core = cpu_find_by_lapic(live_apic_id);
        if (core) {
            return core;
        }
    }

    cpu_core_t *core = cpu_find_by_lapic(cpuid_apic_id);
    if (core) {
        return core;
    }

    return NULL;
#endif
}

bool arch_current_cpu_id(size_t *out) {
    if (!out) {
        return false;
    }

#if defined(__x86_64__)
    cpu_core_t *local_core = (cpu_core_t *)arch_cpu_get_local();
    if (_cpu_ptr_is_core_local(local_core) && local_core->valid && local_core->id < MAX_CORES) {
        *out = local_core->id;
        return true;
    }
#else
    size_t core_id = 0;
    if (gdt_current_core_id(&core_id) && core_id < MAX_CORES) {
        cpu_core_t *core = &cores_local[core_id];
        if (core->valid) {
            *out = core_id;
            return true;
        }
    }
#endif

    cpuid_regs_t regs = { 0 };
    cpuid(1, &regs);
    u32 cpuid_apic_id = (regs.ebx >> 24) & 0xffU;
    u32 live_apic_id = lapic_id();

    if (live_apic_id || live_apic_id == cpuid_apic_id) {
        cpu_core_t *apic_core = cpu_find_by_lapic(live_apic_id);
        if (apic_core && apic_core->valid && apic_core->id < MAX_CORES) {
            *out = apic_core->id;
            return true;
        }
    }

    cpu_core_t *cpuid_core = cpu_find_by_lapic(cpuid_apic_id);
    if (cpuid_core && cpuid_core->valid && cpuid_core->id < MAX_CORES) {
        *out = cpuid_core->id;
        return true;
    }

    return false;
}

unsigned long arch_irq_save(void) {
    return irq_save();
}

void arch_irq_restore(unsigned long flags) {
    irq_restore(flags);
}

bool arch_irq_enabled(void) {
#if defined(__x86_64__)
    u64 flags = 0;
    asm volatile("pushfq\n"
                 "popq %0"
                 : "=r"(flags)
                 :
                 : "memory", "cc");
    return (flags & (1ULL << 9)) != 0;
#else
    u32 flags = 0;
    asm volatile("pushf\n"
                 "pop %0"
                 : "=r"(flags)
                 :
                 : "memory", "cc");
    return (flags & (1U << 9)) != 0;
#endif
}

void arch_cpu_wait(void) {
    if (arch_irq_enabled()) {
        asm volatile("hlt" ::: "memory");
        return;
    }

    asm volatile("sti\n"
                 "hlt\n"
                 "cli"
                 :
                 :
                 : "memory", "cc");
}

void arch_cpu_relax(void) {
    cpu_pause();
}

void arch_resched_self(void) {
    asm volatile("int %0" : : "i"(SMP_SOFT_RESCHED_VECTOR) : "memory");
}

bool arch_resched_cpu(size_t cpu_id) {
    return smp_send_resched(cpu_id);
}

u64 arch_timer_ticks(void) {
    return irq_ticks();
}

u32 arch_timer_hz(void) {
    return irq_timer_hz();
}

static void _wallclock_set_base(u64 seconds, u64 ticks) {
    __atomic_store_n(&wallclock_base_seconds, seconds, __ATOMIC_RELEASE);
    __atomic_store_n(&wallclock_base_ticks, ticks, __ATOMIC_RELEASE);
    __atomic_store_n(&rtc_sync_ticks, ticks, __ATOMIC_RELEASE);
}

static void _wallclock_try_rtc_sync(u64 now_ticks, bool force) {
    u64 hz = irq_timer_hz();
    if (!hz) {
        return;
    }

    u64 base_seconds = __atomic_load_n(&wallclock_base_seconds, __ATOMIC_ACQUIRE);
    u64 base_ticks = __atomic_load_n(&wallclock_base_ticks, __ATOMIC_ACQUIRE);
    u64 last_sync = __atomic_load_n(&rtc_sync_ticks, __ATOMIC_ACQUIRE);
    u64 sync_interval = hz * WALLCLOCK_RTC_RESYNC_SEC;

    if (!force && base_seconds && last_sync && now_ticks >= last_sync && (now_ticks - last_sync) < sync_interval) {
        return;
    }

    u32 expected = 0;
    if (!__atomic_compare_exchange_n(
            &x86_boot.wallclock_sync_inflight,
            &expected,
            1,
            false,
            __ATOMIC_ACQ_REL,
            __ATOMIC_ACQUIRE
        )) {
        return;
    }

    u64 predicted_seconds = base_seconds;
    if (base_seconds && now_ticks >= base_ticks) {
        predicted_seconds = base_seconds + ((now_ticks - base_ticks) / hz);
    }

    u64 rtc_seconds = x86_rtc_unix_seconds();
    if (rtc_seconds) {
        if (base_seconds) {
            if (rtc_seconds < predicted_seconds) {
                rtc_seconds = predicted_seconds;
            } else if (rtc_seconds > (predicted_seconds + 1ULL)) {
                rtc_seconds = predicted_seconds + 1ULL;
            }
        }

        _wallclock_set_base(rtc_seconds, now_ticks);
    } else if (!base_seconds) {
        _wallclock_set_base(now_ticks / hz, now_ticks);
    }

    __atomic_store_n(&x86_boot.wallclock_sync_inflight, 0, __ATOMIC_RELEASE);
}

static u64 _seconds_from_ticks(u64 now_ticks, u64 hz) {
    if (!hz) {
        return 0;
    }

    u64 base_seconds = __atomic_load_n(&wallclock_base_seconds, __ATOMIC_ACQUIRE);
    u64 base_ticks = __atomic_load_n(&wallclock_base_ticks, __ATOMIC_ACQUIRE);

    if (!base_seconds) {
        _wallclock_try_rtc_sync(now_ticks, true);
        base_seconds = __atomic_load_n(&wallclock_base_seconds, __ATOMIC_ACQUIRE);
        base_ticks = __atomic_load_n(&wallclock_base_ticks, __ATOMIC_ACQUIRE);
    }

    if (!base_seconds && hz) {
        _wallclock_set_base(now_ticks / hz, now_ticks);
        base_seconds = __atomic_load_n(&wallclock_base_seconds, __ATOMIC_ACQUIRE);
        base_ticks = __atomic_load_n(&wallclock_base_ticks, __ATOMIC_ACQUIRE);
    }

    u64 seconds = base_seconds;
    if (hz && now_ticks >= base_ticks) {
        seconds = base_seconds + ((now_ticks - base_ticks) / hz);
    }

    return seconds;
}

u64 arch_realtime_ns(void) {
    u64 hz = irq_timer_hz();
    if (!hz) {
        return 0;
    }

    u64 now_ticks = arch_timer_ticks();
    u64 seconds = _seconds_from_ticks(now_ticks, hz);
    u64 base_seconds = __atomic_load_n(&wallclock_base_seconds, __ATOMIC_ACQUIRE);
    u64 base_ticks = __atomic_load_n(&wallclock_base_ticks, __ATOMIC_ACQUIRE);

    u64 delta_ns = 0;
    if (now_ticks >= base_ticks) {
        delta_ns = ((now_ticks - base_ticks) * 1000000000ULL) / hz;
    }

    if (!base_seconds) {
        return (seconds * 1000000000ULL) + delta_ns;
    }

    return (base_seconds * 1000000000ULL) + delta_ns;
}

void arch_wallclock_maintain(void) {
    u64 now_ticks = arch_timer_ticks();
    _wallclock_try_rtc_sync(now_ticks, false);
}

const char *arch_name(void) {
#if defined(__x86_64__)
    return "x86_64";
#else
    return "x86_32";
#endif
}

const char *arch_cpu_name(void) {
    return x86_cpu.name;
}

u64 arch_cpu_khz(void) {
    return tsc_khz();
}

void arch_mem_info(size_t *total, size_t *free) {
    if (total) {
        *total = pmm_total_mem();
    }

    if (free) {
        *free = pmm_free_mem();
    }
}

size_t arch_mem_installed(void) {
    return pmm_total_mem();
}

void arch_syscall_install(int vector, arch_syscall_handler_t handler) {
    set_int_handler(vector, handler);
    configure_int(vector, GDT_KERNEL_CODE, 0, IDT_TRP);
}

void arch_set_kernel_stack(uintptr_t sp) {
    set_tss_stack(sp);
}

#if defined(__i386__)
NORETURN void arch_context_switch_bad_frame(uintptr_t ctx, u32 eip, u32 cs) {
    sched_thread_t *current = sched_is_running() ? sched_current() : NULL;
    panic(
        "bad i386 kernel return frame ctx=%#lx eip=%#x cs=%#x"
        " current=%#lx pid=%d name=%s current_ctx=%#lx stack=%#lx stack_size=%zu",
        (unsigned long)ctx,
        (unsigned int)eip,
        (unsigned int)cs,
        (unsigned long)(uintptr_t)current,
        current ? current->pid : 0,
        current ? current->name : "none",
        current ? (unsigned long)current->context : 0UL,
        current ? (unsigned long)(uintptr_t)current->stack : 0UL,
        current ? current->stack_size : 0U
    );
    __builtin_unreachable();
}
#endif
