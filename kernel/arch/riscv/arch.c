#include <arch/arch.h>
#include <arch/paging.h>
#include <arch/signal.h>
#include <arch/thread.h>
#include <base/units.h>
#include <drivers/manager.h>
#include <errno.h>
#include <fs/ext2.h>
#include <inttypes.h>
#include <libc_ext/string.h>
#include <log/log.h>
#include <parse/fdt.h>
#include <riscv/asm.h>
#include <riscv/boot.h>
#include <riscv/console.h>
#include <riscv/drivers/serial.h>
#include <riscv/mm/heap.h>
#include <riscv/mm/physical.h>
#include <riscv/platform.h>
#include <riscv/mm/virtual.h>
#include <riscv/serial.h>
#include <riscv/trap.h>
#include <riscv/vm.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/console.h>
#include <sys/cpu.h>
#include <sys/disk.h>
#include <sys/framebuffer.h>
#include <sys/logsink.h>
#include <sys/panic.h>
#include <sys/symbols.h>
#include <sys/tty.h>
#include <sys/tty_input.h>

#define LOG_BOOT_HISTORY_CAP   (128 * 1024)
#define BOOT_ROOTFS_SECTOR_SIZE 512
#define RISCV_TIMEBASE_HZ       10000000ULL
#define RISCV_UART_WINDOW_SIZE  PAGE_4KIB
#define RISCV_MMIO_MAX_REGIONS  24
#define RISCV_MMIO_SCAN_MAX     16
#define RISCV_IRQ_SOFT          1
#define RISCV_IRQ_TIMER         5
#define RISCV_IRQ_EXTERNAL      9
#define RISCV_EXC_ILL           2
#define RISCV_EXC_BREAK         3
#define RISCV_EXC_U_ECALL       8
#define RISCV_EXC_INST_PAGE     12
#define RISCV_EXC_LOAD_PAGE     13
#define RISCV_EXC_STORE_PAGE    15
#define RISCV_UART_DEFAULT_IRQ  10

#define RISCV_PLIC_MAX_IRQS       128U
#define RISCV_PLIC_PRIORITY_BASE  0x000000U
#define RISCV_PLIC_ENABLE_BASE    0x002000U
#define RISCV_PLIC_ENABLE_STRIDE  0x000080U
#define RISCV_PLIC_CONTEXT_BASE   0x200000U
#define RISCV_PLIC_CONTEXT_STRIDE 0x001000U
#define RISCV_PLIC_THRESHOLD_OFF  0x0U
#define RISCV_PLIC_CLAIM_OFF      0x4U

#define RISCV_REG_FMT "%#lx"

#define SBI_EXT_BASE  0x10
#define SBI_EXT_TIME  0x54494d45
#define SBI_EXT_SRST  0x53525354
#define SBI_EXT_APHE  0x41504845

#define SBI_BASE_PROBE_EXTENSION 3
#define SBI_SRST_RESET_TYPE_SHUTDOWN 0
#define SBI_SRST_RESET_REASON_NONE   0
#define SBI_APHE_FID_SOFTIRQ_SET   0
#define SBI_APHE_FID_SOFTIRQ_CLEAR 1

typedef struct {
    long error;
    long value;
} sbi_ret_t;

typedef struct {
    u64 paddr;
    size_t size;
} boot_rootfs_t;

typedef struct {
    u64 paddr;
    size_t size;
    uintptr_t vaddr;
} riscv_mmio_region_t;

typedef struct {
    riscv_irq_handler_t handler;
    void *ctx;
} riscv_irq_slot_t;

typedef struct stack_frame {
    struct stack_frame *next;
    uintptr_t ret;
} riscv_stack_frame_t;

typedef struct {
    bool console_ready;
    bool mirror_console_target;
    char history[LOG_BOOT_HISTORY_CAP];
    size_t history_len;
    spinlock_t lock;
} riscv_log_state_t;

typedef struct {
    kernel_args_t args;
    u64 memory_paddr;
    u64 memory_size;
    const void *dtb;
    size_t dtb_size;
    u64 boot_rootfs_paddr;
    size_t boot_rootfs_size;
    bool boot_rootfs_registered;
} riscv_boot_state_t;

typedef struct {
    page_t *kernel_root;
    uintptr_t early_alloc_cursor;
    uintptr_t early_alloc_limit;
    uintptr_t uart_phys_base;
    uintptr_t uart_io_base;
    riscv_mmio_region_t regions[RISCV_MMIO_MAX_REGIONS];
    size_t region_count;
    uintptr_t next_vaddr;
} riscv_mmio_state_t;

typedef struct {
    uintptr_t plic_phys_base;
    size_t plic_span;
    uintptr_t plic_virt_base;
    u32 uart_irq;
    bool plic_ready;
    riscv_irq_slot_t table[RISCV_PLIC_MAX_IRQS];
} riscv_irq_state_t;

typedef struct {
    arch_syscall_handler_t syscall_handler;
    volatile u64 tick_count;
    u32 user_irq_log_count;
    u32 user_fault_log_count;
    u32 user_ill_log_count;
    u32 trap_log_count;
    u32 external_irq_log_count;
} riscv_runtime_state_t;

typedef struct {
    u64 hartid;
    bool hartid_valid;
    bool late_init_pending;
} riscv_cpu_state_t;

typedef struct {
    riscv_boot_state_t boot;
    riscv_log_state_t log;
    riscv_mmio_state_t mmio;
    riscv_irq_state_t irq;
    riscv_runtime_state_t runtime;
    riscv_cpu_state_t cpu[MAX_CORES];
} riscv_arch_state_t;

static ssize_t
_boot_rootfs_read(disk_dev_t *dev, void *dest, size_t offset, size_t bytes) {
    boot_rootfs_t *ram = dev ? dev->private : NULL;
    if (!ram || !dest || offset > ram->size || bytes > ram->size - offset) {
        return -EINVAL;
    }

    memcpy(dest, (void *)(uintptr_t)(ram->paddr + offset), bytes);
    return (ssize_t)bytes;
}

static ssize_t
_boot_rootfs_write(disk_dev_t *dev, void *src, size_t offset, size_t bytes) {
    boot_rootfs_t *ram = dev ? dev->private : NULL;
    if (!ram || !src || offset > ram->size || bytes > ram->size - offset) {
        return -EINVAL;
    }

    memcpy((void *)(uintptr_t)(ram->paddr + offset), src, bytes);
    return (ssize_t)bytes;
}

static disk_interface_t boot_rootfs_interface = {
    .read = _boot_rootfs_read,
    .write = _boot_rootfs_write,
};

uintptr_t riscv_kernel_sp = 0;
uintptr_t riscv_cpu_local_ptr = 0;

static riscv_arch_state_t riscv_arch = {
    .boot = {
        .memory_paddr = RISCV_KERNEL_BASE,
    },
    .log = {
        .lock = SPINLOCK_INIT,
    },
    .mmio = {
        .uart_phys_base = SERIAL_UART0,
        .uart_io_base = SERIAL_UART0,
        .next_vaddr = RISCV_MMIO_BASE,
    },
};

extern char __bss_end;
extern char __stack_top;
extern void riscv_trap_entry(void);

static inline size_t _current_cpu_id(void) {
    cpu_core_t *core = cpu_current();

    if (core && core->id < MAX_CORES) {
        return core->id;
    }

    return 0;
}

static inline riscv_cpu_state_t *_cpu_state(size_t cpu_id) {
    if (cpu_id >= MAX_CORES) {
        return NULL;
    }

    return &riscv_arch.cpu[cpu_id];
}

static void _record_cpu_boot_state(size_t cpu_id, u64 hartid, bool late_init_pending) {
    riscv_cpu_state_t *cpu = _cpu_state(cpu_id);
    if (!cpu) {
        return;
    }

    cpu->hartid = hartid;
    cpu->hartid_valid = true;
    cpu->late_init_pending = late_init_pending;
}

static u64 _cpu_hartid(size_t cpu_id) {
    riscv_cpu_state_t *cpu = _cpu_state(cpu_id);
    if (cpu && cpu->hartid_valid) {
        return cpu->hartid;
    }

    cpu = _cpu_state(0);
    return cpu ? cpu->hartid : 0;
}

static bool _cpu_late_init_pending(size_t cpu_id) {
    riscv_cpu_state_t *cpu = _cpu_state(cpu_id);
    return cpu ? cpu->late_init_pending : false;
}

static void _set_cpu_late_init_pending(size_t cpu_id, bool pending) {
    riscv_cpu_state_t *cpu = _cpu_state(cpu_id);
    if (!cpu) {
        return;
    }

    cpu->late_init_pending = pending;
}

static void _log_history_append(const char *s, size_t len) {
    if (!s || !len || !LOG_BOOT_HISTORY_CAP) {
        return;
    }

    if (len >= LOG_BOOT_HISTORY_CAP) {
        memcpy(
            riscv_arch.log.history,
            s + (len - LOG_BOOT_HISTORY_CAP),
            LOG_BOOT_HISTORY_CAP
        );
        riscv_arch.log.history_len = LOG_BOOT_HISTORY_CAP;
        return;
    }

    if (riscv_arch.log.history_len + len > LOG_BOOT_HISTORY_CAP) {
        size_t drop =
            (riscv_arch.log.history_len + len) - LOG_BOOT_HISTORY_CAP;

        memmove(
            riscv_arch.log.history,
            riscv_arch.log.history + drop,
            riscv_arch.log.history_len - drop
        );
        riscv_arch.log.history_len -= drop;
    }

    memcpy(riscv_arch.log.history + riscv_arch.log.history_len, s, len);
    riscv_arch.log.history_len += len;
}

static void _log_history_replay_console(void) {
    if (!riscv_arch.log.console_ready || !riscv_arch.log.history_len) {
        return;
    }

    console_write_screen(
        TTY_CONSOLE,
        riscv_arch.log.history,
        riscv_arch.log.history_len
    );
}

static void _log_write_early(const char *s, size_t len) {
    if (!s || !len) {
        return;
    }

    send_serial_sized_string(riscv_console_uart_base(), s, len);
}

static void _log_mirror_console(const char *s, size_t len) {
    if (!s || !len || !riscv_arch.log.console_ready) {
        return;
    }

    size_t text_cols = 0;
    size_t text_rows = 0;
    if (!console_get_size(&text_cols, &text_rows) || !text_cols || !text_rows) {
        return;
    }

    riscv_console_set_output_suppressed(true);
    console_write_screen(TTY_CONSOLE, s, len);
    riscv_console_set_output_suppressed(false);
}

static void _log_puts(const char *s) {
    if (!s) {
        return;
    }

    size_t len = strlen(s);
    if (!len) {
        return;
    }

    unsigned long irq_flags = spin_lock_irqsave(&riscv_arch.log.lock);
    _log_history_append(s, len);
    spin_unlock_irqrestore(&riscv_arch.log.lock, irq_flags);

    if (!logsink_is_bound()) {
        _log_write_early(s, len);
        _log_mirror_console(s, len);
        return;
    }

    logsink_write(s, len);

    if (riscv_arch.log.mirror_console_target) {
        _log_mirror_console(s, len);
    }
}

static void _configure_log_sinks(const boot_info_t *info) {
    if (!info) {
        return;
    }

    logsink_reset();
    riscv_arch.log.mirror_console_target = false;

    if (!info->args.console[0]) {
        logsink_add_target("/dev/console");
        return;
    }

    char devices[sizeof(info->args.console)];
    bool want_serial_console = false;
    bool want_console_screen = false;
    bool have_target = false;

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
            have_target = true;

            if (!strcmp(token, "/dev/ttyS0")) {
                want_serial_console = true;
            } else if (!strcmp(token, "/dev/console")) {
                want_console_screen = true;
            }
        }

        if (!next) {
            break;
        }

        cursor = next + 1;
    }

    if (!have_target) {
        logsink_add_target("/dev/console");
        return;
    }

    riscv_arch.log.mirror_console_target =
        want_serial_console && want_console_screen;

    strncpy(devices, info->args.console, sizeof(devices) - 1);
    devices[sizeof(devices) - 1] = '\0';

    cursor = devices;

    while (cursor && *cursor) {
        char *next = strchr(cursor, ',');
        if (next) {
            *next = '\0';
        }

        char *token = strtrim(cursor);
        strtrunc(token);

        if (token[0]) {
            bool skip_console_target =
                riscv_arch.log.mirror_console_target &&
                !strcmp(token, "/dev/console");

            if (!skip_console_target) {
                logsink_add_target(token);
            }
        }

        if (!next) {
            break;
        }

        cursor = next + 1;
    }

    if (!logsink_has_targets()) {
        logsink_add_target("/dev/console");
        riscv_arch.log.mirror_console_target = false;
    }
}

static void
_log_user_trap_once(
    u32 *counter,
    u32 limit,
    const char *label,
    arch_int_state_t *state,
    uintptr_t cause
) {
    if (!counter || !label || !state || !arch_signal_is_user(state)) {
        return;
    }

    u32 seen = __atomic_load_n(counter, __ATOMIC_RELAXED);
    if (seen >= limit) {
        return;
    }

    if (__atomic_fetch_add(counter, 1, __ATOMIC_RELAXED) >= limit) {
        return;
    }

    sched_thread_t *thread = sched_current();
    log_info(
        "%s pid=%ld cause=" RISCV_REG_FMT " sepc=" RISCV_REG_FMT
        " sp=" RISCV_REG_FMT " stval=" RISCV_REG_FMT
        " a7=" RISCV_REG_FMT " a0=" RISCV_REG_FMT " a1=" RISCV_REG_FMT,
        label,
        thread ? (long)thread->pid : 0L,
        (unsigned long)cause,
        (unsigned long)state->s_regs.sepc,
        (unsigned long)state->s_regs.sp,
        (unsigned long)state->s_regs.stval,
        (unsigned long)state->g_regs.a7,
        (unsigned long)state->g_regs.a0,
        (unsigned long)state->g_regs.a1
    );
}

static inline uintptr_t _plic_context_base(size_t cpu_id) {
    return riscv_arch.irq.plic_virt_base +
           RISCV_PLIC_CONTEXT_BASE +
           (((uintptr_t)_cpu_hartid(cpu_id) * 2U) + 1U) *
               RISCV_PLIC_CONTEXT_STRIDE;
}

static inline volatile u32 *_plic_priority_reg(u32 irq) {
    return (volatile u32 *)(
        riscv_arch.irq.plic_virt_base + RISCV_PLIC_PRIORITY_BASE + irq * 4U
    );
}

static inline volatile u32 *_plic_enable_reg(size_t cpu_id, u32 irq) {
    uintptr_t enable_base =
        riscv_arch.irq.plic_virt_base +
        RISCV_PLIC_ENABLE_BASE +
        (((uintptr_t)_cpu_hartid(cpu_id) * 2U) + 1U) *
            RISCV_PLIC_ENABLE_STRIDE;
    return (volatile u32 *)(enable_base + ((irq / 32U) * sizeof(u32)));
}

static inline volatile u32 *_plic_threshold_reg(size_t cpu_id) {
    return (volatile u32 *)(_plic_context_base(cpu_id) + RISCV_PLIC_THRESHOLD_OFF);
}

static inline volatile u32 *_plic_claim_reg(size_t cpu_id) {
    return (volatile u32 *)(_plic_context_base(cpu_id) + RISCV_PLIC_CLAIM_OFF);
}

static void _plic_toggle_irq_for_cpu(size_t cpu_id, u32 irq, bool enable) {
    if (!riscv_arch.irq.plic_ready || !irq || irq >= RISCV_PLIC_MAX_IRQS) {
        return;
    }

    *_plic_priority_reg(irq) = enable ? 1U : 0U;

    volatile u32 *enable_reg = _plic_enable_reg(cpu_id, irq);
    u32 mask = 1U << (irq % 32U);
    u32 value = *enable_reg;

    if (enable) {
        value |= mask;
    } else {
        value &= ~mask;
    }

    *enable_reg = value;
}

static void _plic_toggle_irq(u32 irq, bool enable) {
    size_t max_cpu = core_count;
    if (!max_cpu || max_cpu > MAX_CORES) {
        max_cpu = MAX_CORES;
    }

    bool applied = false;
    for (size_t cpu_id = 0; cpu_id < max_cpu; cpu_id++) {
        riscv_cpu_state_t *cpu = _cpu_state(cpu_id);
        if (!cpu || !cpu->hartid_valid) {
            continue;
        }

        if (!cores_local[cpu_id].valid) {
            continue;
        }

        _plic_toggle_irq_for_cpu(cpu_id, irq, enable);
        applied = true;
    }

    if (!applied) {
        _plic_toggle_irq_for_cpu(0, irq, enable);
    }
}

static void _plic_sync_irqs_for_cpu(size_t cpu_id) {
    if (!riscv_arch.irq.plic_ready) {
        return;
    }

    for (u32 irq = 1; irq < RISCV_PLIC_MAX_IRQS; irq++) {
        if (riscv_arch.irq.table[irq].handler) {
            _plic_toggle_irq_for_cpu(cpu_id, irq, true);
        }
    }
}

static void _plic_init_context(size_t cpu_id) {
    if (!riscv_arch.irq.plic_ready) {
        return;
    }

    uintptr_t enable_base =
        riscv_arch.irq.plic_virt_base +
        RISCV_PLIC_ENABLE_BASE +
        (((uintptr_t)_cpu_hartid(cpu_id) * 2U) + 1U) *
            RISCV_PLIC_ENABLE_STRIDE;

    for (size_t i = 0; i < RISCV_PLIC_MAX_IRQS / 32U; i++) {
        *(volatile u32 *)(enable_base + i * sizeof(u32)) = 0;
    }

    *_plic_threshold_reg(cpu_id) = 0;
    _plic_sync_irqs_for_cpu(cpu_id);
}

static void _serial_drain_input(void) {
    for (size_t i = 0; i < 64; i++) {
        char ch = 0;
        if (!serial_try_receive(riscv_console_uart_base(), &ch)) {
            break;
        }

        tty_input_push(ch);
        riscv_serial_rx_push(ch);
    }
}

static void _uart_irq_handler(u32 irq, void *ctx) {
    (void)irq;
    (void)ctx;
    _serial_drain_input();
}

static void _plic_init(void) {
    if (!riscv_arch.boot.dtb) {
        return;
    }

    if (!riscv_arch.irq.plic_ready) {
        fdt_reg_t reg = {0};
        if (!fdt_find_compatible_reg(
                riscv_arch.boot.dtb,
                "sifive,plic-1.0.0",
                &reg
            ) ||
            !reg.addr || !reg.size) {
            if (!fdt_find_compatible_reg(
                    riscv_arch.boot.dtb,
                    "riscv,plic0",
                    &reg
                ) ||
                !reg.addr || !reg.size) {
                return;
            }
        }

        riscv_arch.irq.plic_phys_base = (uintptr_t)reg.addr;
        riscv_arch.irq.plic_span = (size_t)reg.size;
        riscv_arch.irq.plic_virt_base = (uintptr_t)arch_phys_map(
            riscv_arch.irq.plic_phys_base,
            riscv_arch.irq.plic_span,
            PHYS_MAP_MMIO
        );
        if (!riscv_arch.irq.plic_virt_base) {
            log_warn("failed to map RISC-V PLIC");
            return;
        }

        riscv_arch.irq.plic_ready = true;

        if (riscv_arch.irq.uart_irq) {
            (void)riscv_irq_register(
                riscv_arch.irq.uart_irq,
                _uart_irq_handler,
                NULL
            );
            serial_set_rx_interrupt(riscv_console_uart_base(), true);
        }
    }

    size_t cpu_id = _current_cpu_id();
    _plic_init_context(cpu_id);

}

static bool _plic_handle_external(void) {
    if (!riscv_arch.irq.plic_ready) {
        return false;
    }

    size_t cpu_id = _current_cpu_id();
    u32 irq = *_plic_claim_reg(cpu_id);
    if (!irq) {
        return false;
    }

    if (irq < RISCV_PLIC_MAX_IRQS && riscv_arch.irq.table[irq].handler) {
        riscv_arch.irq.table[irq].handler(irq, riscv_arch.irq.table[irq].ctx);
    } else if (__atomic_fetch_add(
                   &riscv_arch.runtime.external_irq_log_count,
                   1,
                   __ATOMIC_RELAXED
               ) < 8U) {
        log_warn("unhandled RISC-V external irq %u", (unsigned int)irq);
    }

    *_plic_claim_reg(cpu_id) = irq;
    return true;
}

static sbi_ret_t _sbi_call(
    long ext,
    long fid,
    uintptr_t arg0,
    uintptr_t arg1,
    uintptr_t arg2
) {
    register uintptr_t a0 asm("a0") = arg0;
    register uintptr_t a1 asm("a1") = arg1;
    register uintptr_t a2 asm("a2") = arg2;
    register uintptr_t a6 asm("a6") = (uintptr_t)fid;
    register uintptr_t a7 asm("a7") = (uintptr_t)ext;

    asm volatile(
        "ecall"
        : "+r"(a0), "+r"(a1)
        : "r"(a2), "r"(a6), "r"(a7)
        : "memory"
    );

    return (sbi_ret_t){
        .error = (long)a0,
        .value = (long)a1,
    };
}

static void _sbi_set_timer(u64 next) {
#if __riscv_xlen == 64
    (void)_sbi_call(SBI_EXT_TIME, 0, (uintptr_t)next, 0, 0);
#else
    (void)_sbi_call(
        SBI_EXT_TIME,
        0,
        (uintptr_t)(u32)next,
        (uintptr_t)(u32)(next >> 32),
        0
    );
#endif
}

static uintptr_t _align_up(uintptr_t value, size_t align) {
    return ALIGN(value, align ? align : 1);
}

static void *_early_alloc(size_t size, size_t align) {
    if (!size) {
        return NULL;
    }

    uintptr_t cursor =
        _align_up(riscv_arch.mmio.early_alloc_cursor, align ? align : 16);
    uintptr_t next = cursor + ALIGN(size, align ? align : 16);

    if (next < cursor || next > riscv_arch.mmio.early_alloc_limit) {
        panic("RISC-V early allocator exhausted");
    }

    riscv_arch.mmio.early_alloc_cursor = next;
    memset((void *)cursor, 0, next - cursor);
    return (void *)cursor;
}

static page_t _pte_leaf_flags(u64 flags) {
    page_t pte = PT_PRESENT | PT_ACCESSED | PT_DIRTY | PT_READ;

    if (flags & PT_WRITE) {
        pte |= PT_WRITE;
    }

    if (!(flags & PT_NO_EXECUTE)) {
        pte |= PT_EXECUTE;
    }

    if (flags & PT_USER) {
        pte |= PT_USER;
    }

    if (flags & PT_GLOBAL) {
        pte |= PT_GLOBAL;
    }

    return pte;
}

static page_t *_early_walk(page_t *table, size_t index) {
    if (table[index] & PT_PRESENT) {
        return (page_t *)(uintptr_t)page_get_paddr(&table[index]);
    }

    page_t *next = _early_alloc(PAGE_4KIB, PAGE_4KIB);
    table[index] = 0;
    page_set_paddr(&table[index], (u64)(uintptr_t)next);
    table[index] |= PT_PRESENT;
    return next;
}

static void _early_map_page(page_t *root, u64 vaddr, u64 paddr, u64 flags) {
#if __riscv_xlen == 64
    page_t *lvl2 = _early_walk(root, GET_LVL3_INDEX(vaddr));
    page_t *lvl1 = _early_walk(lvl2, GET_LVL2_INDEX(vaddr));
    page_t *entry = &lvl1[GET_LVL1_INDEX(vaddr)];
#else
    page_t *lvl1 = _early_walk(root, GET_LVL2_INDEX(vaddr));
    page_t *entry = &lvl1[GET_LVL1_INDEX(vaddr)];
#endif

    *entry = 0;
    page_set_paddr(entry, paddr);
    *entry |= _pte_leaf_flags(flags);
}

static void _early_map_range(page_t *root, u64 vaddr, u64 paddr, u64 size, u64 flags) {
    u64 base = ALIGN_DOWN(paddr, PAGE_4KIB);
    u64 end = ALIGN(paddr + size, PAGE_4KIB);

    for (u64 cur = base; cur < end; cur += PAGE_4KIB) {
        _early_map_page(root, vaddr + (cur - base), cur, flags);
    }
}

static uintptr_t _mmio_register_region(u64 paddr, size_t size) {
    if (!paddr || !size) {
        return 0;
    }

    u64 base = ALIGN_DOWN(paddr, PAGE_4KIB);
    u64 end = ALIGN(paddr + size, PAGE_4KIB);
    if (end <= base) {
        return 0;
    }

    for (size_t i = 0; i < riscv_arch.mmio.region_count; i++) {
        riscv_mmio_region_t *region = &riscv_arch.mmio.regions[i];
        u64 region_end = region->paddr + region->size;
        if (base >= region->paddr && end <= region_end) {
            return region->vaddr + (uintptr_t)(paddr - region->paddr);
        }
    }

    if (riscv_arch.mmio.region_count >= RISCV_MMIO_MAX_REGIONS) {
        log_warn("RISC-V MMIO region table full");
        return 0;
    }

    uintptr_t vaddr = ALIGN(riscv_arch.mmio.next_vaddr, PAGE_4KIB);
    size_t span = (size_t)(end - base);

    riscv_arch.mmio.regions[riscv_arch.mmio.region_count++] =
        (riscv_mmio_region_t){
        .paddr = base,
        .size = span,
        .vaddr = vaddr,
    };
    riscv_arch.mmio.next_vaddr = vaddr + span;
    return vaddr + (uintptr_t)(paddr - base);
}

static uintptr_t _mmio_translate(u64 paddr, size_t size) {
    if (!paddr) {
        return 0;
    }

    u64 end = paddr + size;
    if (end < paddr) {
        return 0;
    }

    for (size_t i = 0; i < riscv_arch.mmio.region_count; i++) {
        riscv_mmio_region_t *region = &riscv_arch.mmio.regions[i];
        u64 region_end = region->paddr + region->size;
        if (paddr >= region->paddr && end <= region_end) {
            return region->vaddr + (uintptr_t)(paddr - region->paddr);
        }
    }

    return 0;
}

static void _mmio_register_compatible(const void *dtb, const char *compatible) {
    if (!dtb || !compatible || !compatible[0]) {
        return;
    }

    fdt_reg_t regs[RISCV_MMIO_SCAN_MAX];
    size_t count = 0;
    if (!fdt_find_compatible_regs(
            dtb,
            compatible,
            regs,
            sizeof(regs) / sizeof(regs[0]),
            &count
        )) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        if (!regs[i].addr || !regs[i].size) {
            continue;
        }

        (void)_mmio_register_region(regs[i].addr, (size_t)regs[i].size);
    }
}

static void _mmio_map_regions(page_t *root) {
    for (size_t i = 0; i < riscv_arch.mmio.region_count; i++) {
        riscv_mmio_region_t *region = &riscv_arch.mmio.regions[i];
        _early_map_range(
            root,
            region->vaddr,
            region->paddr,
            region->size,
            PT_WRITE | PT_GLOBAL | PT_NO_EXECUTE
        );
    }
}

static void _timer_program_next(void) {
    u64 interval = RISCV_TIMEBASE_HZ / (TIMER_FREQ ? TIMER_FREQ : 1U);
    if (!interval) {
        interval = 1;
    }

    _sbi_set_timer(riscv_read_time() + interval);
}

static bool _handle_user_signal(int signum, arch_int_state_t *state) {
    if (!state || !arch_signal_is_user(state) || !sched_is_running()) {
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

static void _handle_page_fault(arch_int_state_t *state, uintptr_t cause) {
    uintptr_t addr = state ? state->s_regs.stval : 0;
    bool write = cause == RISCV_EXC_STORE_PAGE;
    bool user = arch_signal_is_user(state);

    if (write && user && sched_is_running()) {
        sched_thread_t *thread = sched_current();
        if (thread && thread->user_thread &&
            sched_handle_cow_fault(thread, addr, true)) {
            return;
        }
    }

    if (user) {
        _log_user_trap_once(
            &riscv_arch.runtime.user_fault_log_count,
            8,
            "riscv user page fault",
            state,
            cause
        );
    }

    if (_handle_user_signal(SIGSEGV, state)) {
        return;
    }

    panic_prepare();
    log_fatal(
        "page fault cause=" RISCV_REG_FMT " addr=" RISCV_REG_FMT
        " sepc=" RISCV_REG_FMT,
        (unsigned long)cause,
        (unsigned long)addr,
        state ? (unsigned long)state->s_regs.sepc : 0UL
    );
    panic_dump_state(state);
    panic_halt();
}

static void _register_boot_rootfs(void) {
    if (!riscv_arch.boot.boot_rootfs_paddr ||
        !riscv_arch.boot.boot_rootfs_size ||
        riscv_arch.boot.boot_rootfs_registered) {
        return;
    }

    boot_rootfs_t *rootfs = calloc(1, sizeof(*rootfs));
    disk_dev_t *disk = calloc(1, sizeof(*disk));

    if (!rootfs || !disk) {
        free(rootfs);
        free(disk);
        log_warn("failed to allocate staged rootfs disk");
        return;
    }

    rootfs->paddr = riscv_arch.boot.boot_rootfs_paddr;
    rootfs->size = riscv_arch.boot.boot_rootfs_size;

    disk->name = strdup("ram0");
    disk->type = DISK_VIRTUAL;
    disk->sector_size = BOOT_ROOTFS_SECTOR_SIZE;
    disk->sector_count =
        DIV_ROUND_UP(rootfs->size, (size_t)BOOT_ROOTFS_SECTOR_SIZE);
    disk->interface = &boot_rootfs_interface;
    disk->private = rootfs;

    if (!disk->name || !disk->sector_count || !disk_register(disk)) {
        free(disk->name);
        free(disk);
        free(rootfs);
        log_warn("failed to register staged rootfs");
        return;
    }

    riscv_arch.boot.boot_rootfs_registered = true;
    log_info(
        "registered /dev/%s from boot image (%zu KiB)",
        disk->name,
        rootfs->size / 1024
    );
}

static void _relocate_boot_dtb(boot_info_t *info, uintptr_t *reserved_end) {
    if (!info || !reserved_end || !info->dtb_paddr || !info->dtb_size) {
        return;
    }

    size_t dtb_size = 0;
    if (info->dtb_size <= (u64)(size_t)-1) {
        dtb_size = (size_t)info->dtb_size;
    }

    if (!dtb_size) {
        return;
    }

    uintptr_t src = (uintptr_t)info->dtb_paddr;
    uintptr_t dst = ALIGN(*reserved_end, PAGE_4KIB);
    uintptr_t next = ALIGN(dst + dtb_size, PAGE_4KIB);

    if (next <= dst ||
        next > (uintptr_t)(riscv_arch.boot.memory_paddr +
                           riscv_arch.boot.memory_size)) {
        panic("boot DTB relocation exceeds RAM");
    }

    if (dst != src) {
        memmove((void *)dst, (const void *)src, dtb_size);
    }

    info->dtb_paddr = (u64)dst;
    info->dtb_size = (u64)dtb_size;
    riscv_arch.boot.dtb = (const void *)dst;
    riscv_arch.boot.dtb_size = dtb_size;

    if (!fdt_valid(riscv_arch.boot.dtb)) {
        panic("relocated boot DTB is invalid");
    }

    *reserved_end = next;
}

const kernel_args_t *arch_init(void *boot_info_ptr) {
    boot_info_t *info = boot_info_ptr;

    if (!info) {
        panic("boot info missing");
    }

    memcpy(&riscv_arch.boot.args, &info->args, sizeof(riscv_arch.boot.args));
    _configure_log_sinks(info);

    riscv_arch.mmio.uart_phys_base =
        info->uart_paddr ? (uintptr_t)info->uart_paddr : SERIAL_UART0;
    _record_cpu_boot_state(0, info->hartid, true);
    riscv_arch.boot.dtb =
        info->dtb_paddr ? (const void *)(uintptr_t)info->dtb_paddr : NULL;
    riscv_arch.boot.dtb_size = 0;
    if (info->dtb_size <= (u64)(size_t)-1) {
        riscv_arch.boot.dtb_size = (size_t)info->dtb_size;
    }
    riscv_arch.irq.uart_irq = 0;
    if (riscv_arch.boot.dtb) {
        (void)fdt_find_compatible_irq(
            riscv_arch.boot.dtb,
            "ns16550a",
            &riscv_arch.irq.uart_irq
        );
    }
    if (!riscv_arch.irq.uart_irq &&
        riscv_arch.mmio.uart_phys_base == SERIAL_UART0) {
        riscv_arch.irq.uart_irq = RISCV_UART_DEFAULT_IRQ;
    }

    riscv_arch.mmio.region_count = 0;
    riscv_arch.mmio.next_vaddr = RISCV_MMIO_BASE;
    riscv_arch.mmio.uart_io_base = _mmio_register_region(
        riscv_arch.mmio.uart_phys_base,
        RISCV_UART_WINDOW_SIZE
    );
    if (!riscv_arch.mmio.uart_io_base) {
        panic("failed to register UART MMIO window");
    }
    riscv_console_set_uart_base(riscv_arch.mmio.uart_phys_base);

    log_init(_log_puts);
    log_set_lvl(
        info->args.debug == DEBUG_ALL ? LOG_DEBUG : LOG_INFO
    );

#if __riscv_xlen == 64
    log_info("apheleiaOS kernel (riscv_64) booting");
#else
    log_info("apheleiaOS kernel (riscv_32) booting");
#endif

    riscv_arch.boot.memory_paddr =
        info->memory_paddr ? info->memory_paddr : RISCV_KERNEL_BASE;
    riscv_arch.boot.memory_size =
        info->memory_size ? info->memory_size : (256ULL * MIB);
    riscv_arch.boot.boot_rootfs_paddr = info->boot_rootfs_paddr;
    riscv_arch.boot.boot_rootfs_size = 0;
    if (info->boot_rootfs_size <= (u64)(size_t)-1) {
        riscv_arch.boot.boot_rootfs_size = (size_t)info->boot_rootfs_size;
    }
    riscv_arch.boot.boot_rootfs_registered = false;

    if (riscv_arch.boot.boot_rootfs_paddr && riscv_arch.boot.boot_rootfs_size) {
        log_info(
            "staged rootfs at %#llx (%zu KiB)",
            (unsigned long long)riscv_arch.boot.boot_rootfs_paddr,
            riscv_arch.boot.boot_rootfs_size / 1024
        );
    }

    cpu_init_boot();
    arch_cpu_set_local(&cores_local[0]);
    arch_set_kernel_stack((uintptr_t)&__stack_top);
    riscv_write_sstatus(riscv_read_sstatus() | SSTATUS_SUM);

    uintptr_t reserved_end = (uintptr_t)&__bss_end;
    uintptr_t info_end = (uintptr_t)info + sizeof(*info);

    if (info_end > reserved_end) {
        reserved_end = info_end;
    }

    _relocate_boot_dtb(info, &reserved_end);
    if (riscv_arch.boot.boot_rootfs_paddr &&
        riscv_arch.boot.boot_rootfs_size) {
        uintptr_t rootfs_end = (uintptr_t)(
            riscv_arch.boot.boot_rootfs_paddr +
            riscv_arch.boot.boot_rootfs_size
        );
        if (rootfs_end > reserved_end) {
            reserved_end = rootfs_end;
        }
    }

    riscv_arch.mmio.early_alloc_cursor = ALIGN(reserved_end, PAGE_4KIB);
    riscv_arch.mmio.early_alloc_limit = (uintptr_t)(
        riscv_arch.boot.memory_paddr + riscv_arch.boot.memory_size
    );

    riscv_arch.mmio.kernel_root = _early_alloc(PAGE_4KIB, PAGE_4KIB);
    _early_map_range(
        riscv_arch.mmio.kernel_root,
        riscv_arch.boot.memory_paddr,
        riscv_arch.boot.memory_paddr,
        riscv_arch.boot.memory_size,
        PT_WRITE | PT_GLOBAL
    );

    _mmio_register_compatible(riscv_arch.boot.dtb, "virtio,mmio");
    _mmio_map_regions(riscv_arch.mmio.kernel_root);

    riscv_trap_init();
    riscv_vm_init_kernel(riscv_arch.mmio.kernel_root);
    arch_vm_switch(arch_vm_kernel());
    riscv_console_set_uart_base(riscv_arch.mmio.uart_io_base);
    _plic_init();

    pmm_init(
        riscv_arch.boot.memory_paddr,
        riscv_arch.boot.memory_size,
        riscv_arch.mmio.early_alloc_cursor
    );
    heap_init();
    arch_init_alloc();
    pmm_ref_init();
    if (!pmm_ref_ready()) {
        panic("PMM refcount table unavailable");
    }

    framebuffer_set_info(NULL);
    riscv_console_backend_init(riscv_arch.mmio.uart_io_base);
    console_init(info);
    riscv_arch.log.console_ready = true;

    size_t text_cols = 0;
    size_t text_rows = 0;
    if (console_get_size(&text_cols, &text_rows) && text_cols && text_rows) {
        riscv_console_set_output_suppressed(true);
        arch_log_replay_console();
        riscv_console_set_output_suppressed(false);
    }

    if (!driver_registry_init()) {
        log_warn("driver registry init failed");
    } else {
        driver_load_stage(DRIVER_STAGE_ARCH_EARLY);
    }

    riscv_write_scounteren(
        RISCV_COUNTEREN_CY | RISCV_COUNTEREN_TM | RISCV_COUNTEREN_IR
    );
    _set_cpu_late_init_pending(0, true);

#if __riscv_xlen == 64
    log_info("apheleiaOS kernel (riscv_64) booted");
#else
    log_info("apheleiaOS kernel (riscv_32) booted");
#endif
    return &riscv_arch.boot.args;
}

void arch_storage_init(void) {
    driver_load_stage(DRIVER_STAGE_STORAGE);
    _register_boot_rootfs();
}

void arch_late_init(void) {
    size_t cpu_id = _current_cpu_id();
    if (!_cpu_late_init_pending(cpu_id)) {
        return;
    }

    _timer_program_next();
    riscv_set_sie_bits(
        SIE_SSIE | SIE_STIE |
        (riscv_arch.irq.plic_ready ? SIE_SEIE : 0)
    );
    _set_cpu_late_init_pending(cpu_id, false);
}

void arch_log_replay_console(void) {
    unsigned long irq_flags = spin_lock_irqsave(&riscv_arch.log.lock);
    _log_history_replay_console();
    spin_unlock_irqrestore(&riscv_arch.log.lock, irq_flags);
}

void arch_smp_init(void) {
    // RISC-V stays single-hart until real SMP bring-up lands.
}

bool arch_supports_nx(void) {
    return true;
}

void *arch_phys_map(u64 paddr, size_t size, u32 flags) {
    if (paddr >= riscv_arch.boot.memory_paddr &&
        paddr + size <= riscv_arch.boot.memory_paddr + riscv_arch.boot.memory_size) {
        return (void *)(uintptr_t)paddr;
    }

    uintptr_t mmio = _mmio_translate(paddr, size);
    if (mmio) {
        return (void *)mmio;
    }

    if ((flags & PHYS_MAP_MMIO) && size) {
        mmio = _mmio_register_region(paddr, size);
        if (mmio && riscv_arch.mmio.kernel_root && riscv_arch.mmio.region_count) {
            riscv_mmio_region_t *region =
                &riscv_arch.mmio.regions[riscv_arch.mmio.region_count - 1];
            map_region(
                riscv_arch.mmio.kernel_root,
                DIV_ROUND_UP(region->size, PAGE_4KIB),
                region->vaddr,
                region->paddr,
                PT_WRITE | PT_GLOBAL | PT_NO_EXECUTE
            );
            sfence_vma();
            return (void *)mmio;
        }
    }

    return NULL;
}

void arch_phys_unmap(void *vaddr, size_t size) {
    (void)vaddr;
    (void)size;
}

bool arch_phys_copy(u64 dst_paddr, u64 src_paddr, size_t size) {
    void *dst = arch_phys_map(dst_paddr, size, 0);
    void *src = arch_phys_map(src_paddr, size, 0);
    if (!dst || !src) {
        return false;
    }

    memcpy(dst, src, size);
    return true;
}

bool arch_phys_map_can_persist(void) {
    return true;
}

void arch_dump_stack_trace(void) {
    riscv_stack_frame_t *frame =
        (riscv_stack_frame_t *)__builtin_frame_address(0);

    if (frame) {
        frame = frame->next;
    }

    log_info("stack trace");

    for (size_t i = 0; frame && i < 32; i++) {
        uintptr_t ret = frame->ret;
        symbol_entry_t *sym = resolve_symbol((u64)ret);

        if (!sym) {
            log_info("<%#llx> (unknown symbol)", (unsigned long long)ret);
        } else {
            u64 offset = (u64)ret - sym->addr;
            log_info(
                "<%#llx> %s+%#llx",
                (unsigned long long)ret,
                sym->name,
                (unsigned long long)offset
            );
        }

        if (frame->next <= frame) {
            break;
        }

        frame = frame->next;
    }
}

void arch_dump_registers(const arch_int_state_t *state) {
    if (!state) {
        return;
    }

    log_fatal(
        "sepc=" RISCV_REG_FMT " sp=" RISCV_REG_FMT " sstatus=" RISCV_REG_FMT
        " scause=" RISCV_REG_FMT " stval=" RISCV_REG_FMT,
        (unsigned long)state->s_regs.sepc,
        (unsigned long)state->s_regs.sp,
        (unsigned long)state->s_regs.sstatus,
        (unsigned long)state->s_regs.scause,
        (unsigned long)state->s_regs.stval
    );
}

void arch_tlb_flush(uintptr_t addr) {
    (void)addr;
    sfence_vma();
}

void arch_cpu_set_local(void *ptr) {
    riscv_cpu_local_ptr = (uintptr_t)ptr;
    riscv_write_tp((uintptr_t)ptr);
}

void *arch_cpu_get_local(void) {
    uintptr_t ptr = riscv_read_tp();
    if (!ptr) {
        ptr = riscv_cpu_local_ptr;
    }
    return (void *)ptr;
}

unsigned long arch_irq_save(void) {
    unsigned long flags = riscv_read_sstatus();
    riscv_disable_irqs();
    return flags;
}

void arch_irq_restore(unsigned long flags) {
    unsigned long cur = riscv_read_sstatus();
    if (flags & SSTATUS_SIE) {
        cur |= SSTATUS_SIE;
    } else {
        cur &= ~SSTATUS_SIE;
    }
    riscv_write_sstatus(cur);
}

bool arch_irq_enabled(void) {
    return (riscv_read_sstatus() & SSTATUS_SIE) != 0;
}

void arch_cpu_wait(void) {
    asm volatile("wfi" ::: "memory");
}

void arch_cpu_relax(void) {
    asm volatile("nop");
}

void arch_resched_self(void) {
    riscv_resched_trap();
}

bool arch_resched_cpu(size_t cpu_id) {
    cpu_core_t *core = cpu_current();
    size_t self_cpu = core ? core->id : 0;

    if (cpu_id == self_cpu) {
        arch_resched_self();
        return true;
    }
    return false;
}

u64 arch_timer_ticks(void) {
    return __atomic_load_n(&riscv_arch.runtime.tick_count, __ATOMIC_ACQUIRE);
}

u32 arch_timer_hz(void) {
    return TIMER_FREQ ? TIMER_FREQ : 1U;
}

u64 arch_realtime_ns(void) {
    return (riscv_read_time() * 1000000000ULL) / RISCV_TIMEBASE_HZ;
}

const char *arch_name(void) {
#if __riscv_xlen == 64
    return "riscv_64";
#else
    return "riscv_32";
#endif
}

const char *arch_cpu_name(void) {
    return "qemu-virt";
}

u64 arch_cpu_khz(void) {
    return RISCV_TIMEBASE_HZ / 1000ULL;
}

const void *riscv_boot_dtb(void) {
    if (riscv_arch.boot.dtb &&
        riscv_arch.boot.dtb_size &&
        fdt_valid(riscv_arch.boot.dtb)) {
        return riscv_arch.boot.dtb;
    }

    return NULL;
}

u64 riscv_boot_hartid(void) {
    return _cpu_hartid(0);
}

void arch_mem_info(size_t *total, size_t *free_mem) {
    if (total) {
        *total = pmm_total_mem();
    }
    if (free_mem) {
        *free_mem = pmm_free_mem();
    }
}

void arch_syscall_install(int vector, arch_syscall_handler_t handler) {
    (void)vector;
    riscv_arch.runtime.syscall_handler = handler;
}

void arch_set_kernel_stack(uintptr_t sp) {
    riscv_kernel_sp = sp;
    riscv_write_sscratch(sp);
}

void arch_panic_enter(void) {
    if (riscv_console_uart_base()) {
        serial_set_rx_interrupt(riscv_console_uart_base(), false);
    }
    logsink_unbind_devices();
    console_panic();
}

void arch_fpu_init(void *buf) {
    (void)buf;
}

void arch_fpu_save(void *buf) {
    (void)buf;
}

void arch_fpu_restore(const void *buf) {
    (void)buf;
}

ssize_t arch_log_ring_read(void *buf, size_t offset, size_t len) {
    if (!buf) {
        return -1;
    }

    if (!len) {
        return 0;
    }

    unsigned long irq_flags = spin_lock_irqsave(&riscv_arch.log.lock);
    size_t history_len = riscv_arch.log.history_len;

    if (offset >= history_len) {
        spin_unlock_irqrestore(&riscv_arch.log.lock, irq_flags);
        return 0;
    }

    size_t copy_len = history_len - offset;
    if (copy_len > len) {
        copy_len = len;
    }

    memcpy(buf, riscv_arch.log.history + offset, copy_len);
    spin_unlock_irqrestore(&riscv_arch.log.lock, irq_flags);
    return (ssize_t)copy_len;
}

size_t arch_log_ring_size(void) {
    unsigned long irq_flags = spin_lock_irqsave(&riscv_arch.log.lock);
    size_t history_len = riscv_arch.log.history_len;
    spin_unlock_irqrestore(&riscv_arch.log.lock, irq_flags);
    return history_len;
}

void panic_prepare(void) {
    arch_panic_enter();
    riscv_disable_irqs();
}

void panic_halt(void) {
    riscv_disable_irqs();
    halt();
}

void riscv_trap_init(void) {
    if (((uintptr_t)riscv_trap_entry & 0x3U) != 0) {
        panic("RISC-V trap entry misaligned");
    }

    riscv_write_stvec((uintptr_t)riscv_trap_entry);
}

void riscv_handle_trap(arch_int_state_t *state) {
    if (!state) {
        panic("missing trap state");
    }

#if __riscv_xlen == 64
    bool interrupt = (state->s_regs.scause >> 63) != 0;
    uintptr_t cause = state->s_regs.scause & ~(1UL << 63);
#else
    bool interrupt = (state->s_regs.scause >> 31) != 0;
    uintptr_t cause = state->s_regs.scause & ~(1UL << 31);
#endif

    if (interrupt) {
        switch (cause) {
        case RISCV_IRQ_SOFT:
            riscv_clear_sip_bits(SIP_SSIP);
            sched_resched_softirq(state);
            return;
        case RISCV_IRQ_TIMER:
            __atomic_add_fetch(
                &riscv_arch.runtime.tick_count,
                1,
                __ATOMIC_RELAXED
            );
            _timer_program_next();
            _serial_drain_input();
            sched_tick(state);
            return;
        case RISCV_IRQ_EXTERNAL:
            if (_plic_handle_external()) {
                return;
            }
            return;
        default:
            return;
        }
    }

    switch (cause) {
    case RISCV_EXC_BREAK:
        if (
            !arch_signal_is_user(state) &&
            (uintptr_t)state->s_regs.sepc == (uintptr_t)riscv_resched_trap
        ) {
            state->s_regs.sepc += 4;
            sched_resched_softirq(state);
            return;
        }
        break;
    case RISCV_EXC_U_ECALL:
        sched_capture_context(state);
        state->s_regs.sepc += 4;
        if (riscv_arch.runtime.syscall_handler) {
            unsigned long irq_flags = arch_irq_save();

            // The generic scheduler expects blocking syscalls to be
            // preemptible while they wait in kernel context.
            riscv_enable_irqs();
            riscv_arch.runtime.syscall_handler(state);
            arch_irq_restore(irq_flags);
        } else {
            state->g_regs.a0 = (uintptr_t)-ENOSYS;
        }
        return;
    case RISCV_EXC_INST_PAGE:
    case RISCV_EXC_LOAD_PAGE:
    case RISCV_EXC_STORE_PAGE:
        _handle_page_fault(state, cause);
        return;
    case RISCV_EXC_ILL:
        _log_user_trap_once(
            &riscv_arch.runtime.user_ill_log_count,
            8,
            "riscv user illegal instruction",
            state,
            cause
        );
        if (_handle_user_signal(SIGILL, state)) {
            return;
        }
        panic_prepare();
        log_fatal(
            "illegal instruction at " RISCV_REG_FMT,
            (unsigned long)state->s_regs.sepc
        );
        panic_dump_state(state);
        panic_halt();
        return;
    default:
        panic_prepare();
        log_fatal(
            "unhandled trap cause=" RISCV_REG_FMT " sepc=" RISCV_REG_FMT
            " stval=" RISCV_REG_FMT,
            (unsigned long)cause,
            (unsigned long)state->s_regs.sepc,
            (unsigned long)state->s_regs.stval
        );
        panic_dump_state(state);
        panic_halt();
        return;
    }
}

bool riscv_irq_register(u32 irq, riscv_irq_handler_t handler, void *ctx) {
    if (!irq || irq >= RISCV_PLIC_MAX_IRQS || !handler) {
        return false;
    }

    unsigned long flags = arch_irq_save();
    riscv_arch.irq.table[irq].handler = handler;
    riscv_arch.irq.table[irq].ctx = ctx;
    _plic_toggle_irq(irq, true);
    arch_irq_restore(flags);
    return true;
}

void riscv_irq_unregister(u32 irq) {
    if (!irq || irq >= RISCV_PLIC_MAX_IRQS) {
        return;
    }

    unsigned long flags = arch_irq_save();
    _plic_toggle_irq(irq, false);
    riscv_arch.irq.table[irq].handler = NULL;
    riscv_arch.irq.table[irq].ctx = NULL;
    arch_irq_restore(flags);
}
