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
#include <riscv/arch_paging.h>
#include <riscv/asm.h>
#include <riscv/boot.h>
#include <riscv/console.h>
#include <riscv/drivers/serial.h>
#include <riscv/mm/heap.h>
#include <riscv/mm/physical.h>
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

#define LOG_BOOT_HISTORY_CAP    (128 * 1024)
#define BOOT_ROOTFS_SECTOR_SIZE 512
#define RISCV_TIMEBASE_HZ       10000000ULL
#define RISCV_UART_WINDOW_SIZE  PAGE_4KIB
#define RISCV_MMIO_MAX_REGIONS  24
#define RISCV_UART_DEFAULT_IRQ  10

#define RISCV_IRQ_SOFT     1
#define RISCV_IRQ_TIMER    5
#define RISCV_IRQ_EXTERNAL 9

#define RISCV_EXC_ILL        2
#define RISCV_EXC_BREAK      3
#define RISCV_EXC_U_ECALL    8
#define RISCV_EXC_INST_PAGE  12
#define RISCV_EXC_LOAD_PAGE  13
#define RISCV_EXC_STORE_PAGE 15

#define RISCV_PLIC_MAX_IRQS       128U
#define RISCV_PLIC_PRIORITY_BASE  0x000000U
#define RISCV_PLIC_ENABLE_BASE    0x002000U
#define RISCV_PLIC_ENABLE_STRIDE  0x000080U
#define RISCV_PLIC_CONTEXT_BASE   0x200000U
#define RISCV_PLIC_CONTEXT_STRIDE 0x001000U
#define RISCV_PLIC_THRESHOLD_OFF  0x0U
#define RISCV_PLIC_CLAIM_OFF      0x4U

#define RISCV_REG_FMT "%#lx"

typedef struct {
    u64 paddr;
    size_t size;
} boot_rootfs_t;

typedef struct {
    u64 paddr;
    size_t size;
    uintptr_t vaddr;
} mmio_region_t;

typedef void (*irq_handler_t)(u32 irq, void *ctx);

typedef struct {
    irq_handler_t handler;
    void *ctx;
} irq_slot_t;

typedef struct stack_frame {
    struct stack_frame *next;
    uintptr_t ret;
} stack_frame_t;

static kernel_args_t boot_args = {0};
static u64 boot_memory_paddr = RISCV_KERNEL_BASE;
static u64 boot_memory_size = 0;
static const void *boot_dtb = NULL;
static size_t boot_dtb_size = 0;
static u64 boot_rootfs_paddr = 0;
static size_t boot_rootfs_size = 0;
static bool boot_rootfs_registered = false;

static bool log_console_ready = false;
static bool log_mirror_console = false;
static char log_history[LOG_BOOT_HISTORY_CAP];
static size_t log_history_len = 0;
static spinlock_t log_lock = SPINLOCK_INIT;

static page_t *mmio_root = NULL;
static uintptr_t early_cursor = 0;
static uintptr_t early_limit = 0;
static uintptr_t uart_phys = SERIAL_UART0;
static uintptr_t uart_virt = SERIAL_UART0;
static mmio_region_t mmio_regions[RISCV_MMIO_MAX_REGIONS];
static size_t mmio_region_count = 0;
static uintptr_t mmio_next_vaddr = RISCV_MMIO_BASE;

static uintptr_t plic_phys = 0;
static size_t plic_span = 0;
static uintptr_t plic_virt = 0;
static u32 uart_irq = 0;
static bool plic_ready = false;
static irq_slot_t irq_table[RISCV_PLIC_MAX_IRQS];

static arch_syscall_handler_t syscall_handler = NULL;
static volatile u64 tick_count = 0;
static u32 user_fault_log_count = 0;
static u32 user_ill_log_count = 0;
static u32 timer_arm_log_count = 0;
static u32 soft_irq_log_count = 0;
static u32 timer_irq_log_count = 0;
static u32 trap_log_count = 0;
static u32 ext_irq_claim_log_count = 0;
static u32 ext_irq_log_count = 0;

static u64 cpu_hartid[MAX_CORES];
static bool cpu_hartid_valid[MAX_CORES];
static bool cpu_late_init[MAX_CORES];

uintptr_t kernel_sp = 0;
uintptr_t cpu_local_ptr = 0;

extern char __bss_end;
extern char __stack_top;
extern void trap_entry(void);

static bool irq_register(u32 irq, irq_handler_t handler, void *ctx);

static inline size_t _current_cpu_id(void) {
    cpu_core_t *core = cpu_current();
    return (core && core->id < MAX_CORES) ? core->id : 0;
}

static u64 _cpu_hartid(size_t cpu_id) {
    if (cpu_id < MAX_CORES && cpu_hartid_valid[cpu_id]) {
        return cpu_hartid[cpu_id];
    }
    return cpu_hartid[0];
}

static void _log_history_append(const char *s, size_t len) {
    if (!s || !len || !LOG_BOOT_HISTORY_CAP) {
        return;
    }

    if (len >= LOG_BOOT_HISTORY_CAP) {
        memcpy(log_history, s + (len - LOG_BOOT_HISTORY_CAP), LOG_BOOT_HISTORY_CAP);
        log_history_len = LOG_BOOT_HISTORY_CAP;
        return;
    }

    if (log_history_len + len > LOG_BOOT_HISTORY_CAP) {
        size_t drop = (log_history_len + len) - LOG_BOOT_HISTORY_CAP;
        memmove(log_history, log_history + drop, log_history_len - drop);
        log_history_len -= drop;
    }

    memcpy(log_history + log_history_len, s, len);
    log_history_len += len;
}

static void _log_history_replay_console(void) {
    if (!log_console_ready || !log_history_len) {
        return;
    }

    console_write_screen(TTY_CONSOLE, log_history, log_history_len);
}

static void _log_write_early(const char *s, size_t len) {
    if (!s || !len) {
        return;
    }

    send_serial_sized_string(uart_console_base(), s, len);
}

static void _log_mirror_console(const char *s, size_t len) {
    if (!s || !len || !log_console_ready) {
        return;
    }

    size_t text_cols = 0, text_rows = 0;
    if (!console_get_size(&text_cols, &text_rows) || !text_cols || !text_rows) {
        return;
    }

    uart_console_set_suppressed(true);
    console_write_screen(TTY_CONSOLE, s, len);
    uart_console_set_suppressed(false);
}

static void _log_puts(const char *s) {
    if (!s) {
        return;
    }

    size_t len = strlen(s);
    if (!len) {
        return;
    }

    unsigned long irq_flags = spin_lock_irqsave(&log_lock);
    _log_history_append(s, len);
    spin_unlock_irqrestore(&log_lock, irq_flags);

    if (!logsink_is_bound()) {
        _log_write_early(s, len);
        _log_mirror_console(s, len);
        return;
    }

    logsink_write(s, len);

    if (log_mirror_console) {
        _log_mirror_console(s, len);
    }
}

static void _configure_log_sinks(const boot_info_t *info) {
    logsink_reset();
    log_mirror_console = false;

    if (!info || !info->args.console[0]) {
        logsink_add_target("/dev/console");
        return;
    }

    char buf[sizeof(info->args.console)];
    bool want_serial = false, want_screen = false;

    strncpy(buf, info->args.console, sizeof(buf));
    for (char *p = buf; p && *p;) {
        char *sep = strchr(p, ',');
        if (sep) { *sep = '\0'; }
        char *tok = strtrim(p);
        strtrunc(tok);
        if (!strcmp(tok, "/dev/ttyS0"))   { want_serial = true; }
        if (!strcmp(tok, "/dev/console")) { want_screen = true; }
        p = sep ? sep + 1 : NULL;
    }

    log_mirror_console = want_serial && want_screen;

    strncpy(buf, info->args.console, sizeof(buf));
    bool any = false;
    for (char *p = buf; p && *p;) {
        char *sep = strchr(p, ',');
        if (sep) { *sep = '\0'; }
        char *tok = strtrim(p);
        strtrunc(tok);
        p = sep ? sep + 1 : NULL;
        if (!tok[0]) { continue; }
        if (log_mirror_console && !strcmp(tok, "/dev/console")) { continue; }
        logsink_add_target(tok);
        any = true;
    }

    if (!any) {
        logsink_add_target("/dev/console");
        log_mirror_console = false;
    }
}

static void _log_boot_info(const boot_info_t *info) {
    if (!info) {
        return;
    }

    log_info(
        "boot info hart=%llu uart=%#llx dtb=%#llx (%zu bytes) memory=%#llx (%#llx bytes) rootfs=%#llx (%zu bytes)",
        (unsigned long long)info->hartid,
        (unsigned long long)info->uart_paddr,
        (unsigned long long)info->dtb_paddr,
        boot_dtb_size,
        (unsigned long long)boot_memory_paddr,
        (unsigned long long)boot_memory_size,
        (unsigned long long)boot_rootfs_paddr,
        boot_rootfs_size
    );
    log_info(
        "boot args debug=%u video=%u stage_rootfs=%u console='%s' font='%s'",
        (unsigned int)info->args.debug,
        (unsigned int)info->args.video,
        (unsigned int)info->args.stage_rootfs,
        info->args.console,
        info->args.font
    );
}

static void
_log_user_trap_once(
    u32 *counter,
    u32 limit,
    const char *label,
    arch_int_state_t *frame,
    uintptr_t cause
) {
    if (!counter || !label || !frame || !arch_signal_is_user(frame)) {
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
        (unsigned long)frame->s_regs.sepc,
        (unsigned long)frame->s_regs.sp,
        (unsigned long)frame->s_regs.stval,
        (unsigned long)frame->g_regs.a7,
        (unsigned long)frame->g_regs.a0,
        (unsigned long)frame->g_regs.a1
    );
}

static void
_log_trap_once(
    u32 *counter,
    u32 limit,
    const char *label,
    arch_int_state_t *frame,
    uintptr_t cause
) {
    if (!counter || !label || !frame) {
        return;
    }

    u32 seen = __atomic_load_n(counter, __ATOMIC_RELAXED);
    if (seen >= limit) {
        return;
    }

    if (__atomic_fetch_add(counter, 1, __ATOMIC_RELAXED) >= limit) {
        return;
    }

    size_t cpu_id = _current_cpu_id();
    log_info(
        "%s cpu=%zu hart=%llu cause=" RISCV_REG_FMT " sepc=" RISCV_REG_FMT
        " stval=" RISCV_REG_FMT " sp=" RISCV_REG_FMT
        " sstatus=" RISCV_REG_FMT " a0=" RISCV_REG_FMT
        " a1=" RISCV_REG_FMT " a7=" RISCV_REG_FMT " user=%s",
        label,
        cpu_id,
        (unsigned long long)_cpu_hartid(cpu_id),
        (unsigned long)cause,
        (unsigned long)frame->s_regs.sepc,
        (unsigned long)frame->s_regs.stval,
        (unsigned long)frame->s_regs.sp,
        (unsigned long)frame->s_regs.sstatus,
        (unsigned long)frame->g_regs.a0,
        (unsigned long)frame->g_regs.a1,
        (unsigned long)frame->g_regs.a7,
        arch_signal_is_user(frame) ? "yes" : "no"
    );
}

static inline uintptr_t _plic_context_base(size_t cpu_id) {
    uintptr_t smode_ctx = _cpu_hartid(cpu_id) * 2U + 1U; // S-mode context = hartid*2 + 1
    return plic_virt + RISCV_PLIC_CONTEXT_BASE + smode_ctx * RISCV_PLIC_CONTEXT_STRIDE;
}

static inline volatile u32 *_plic_priority_reg(u32 irq) {
    return (volatile u32 *)(plic_virt + RISCV_PLIC_PRIORITY_BASE + irq * 4U);
}

static inline volatile u32 *_plic_enable_reg(size_t cpu_id, u32 irq) {
    uintptr_t smode_ctx = _cpu_hartid(cpu_id) * 2U + 1U;
    uintptr_t enable_base = plic_virt + RISCV_PLIC_ENABLE_BASE + smode_ctx * RISCV_PLIC_ENABLE_STRIDE;
    return (volatile u32 *)(enable_base + (irq / 32U) * sizeof(u32));
}

static inline volatile u32 *_plic_threshold_reg(size_t cpu_id) {
    return (volatile u32 *)(_plic_context_base(cpu_id) + RISCV_PLIC_THRESHOLD_OFF);
}

static inline volatile u32 *_plic_claim_reg(size_t cpu_id) {
    return (volatile u32 *)(_plic_context_base(cpu_id) + RISCV_PLIC_CLAIM_OFF);
}

static void _plic_toggle_irq_for_cpu(size_t cpu_id, u32 irq, bool enable) {
    if (!plic_ready || !irq || irq >= RISCV_PLIC_MAX_IRQS) {
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
    size_t max_cpu = (core_count && core_count <= MAX_CORES) ? core_count : MAX_CORES;
    bool applied = false;

    for (size_t cpu_id = 0; cpu_id < max_cpu; cpu_id++) {
        if (!cpu_hartid_valid[cpu_id] || !cores_local[cpu_id].valid) {
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
    if (!plic_ready) {
        return;
    }

    for (u32 irq = 1; irq < RISCV_PLIC_MAX_IRQS; irq++) {
        if (irq_table[irq].handler) {
            _plic_toggle_irq_for_cpu(cpu_id, irq, true);
        }
    }
}

static void _plic_init_context(size_t cpu_id) {
    if (!plic_ready) {
        return;
    }

    uintptr_t smode_ctx = _cpu_hartid(cpu_id) * 2U + 1U;
    uintptr_t enable_base = plic_virt + RISCV_PLIC_ENABLE_BASE + smode_ctx * RISCV_PLIC_ENABLE_STRIDE;

    for (size_t i = 0; i < RISCV_PLIC_MAX_IRQS / 32U; i++) {
        *(volatile u32 *)(enable_base + i * sizeof(u32)) = 0;
    }

    *_plic_threshold_reg(cpu_id) = 0;
    _plic_sync_irqs_for_cpu(cpu_id);
}

static void _serial_drain_input(void) {
    for (size_t i = 0; i < 64; i++) {
        char ch = 0;
        if (!serial_try_receive(uart_console_base(), &ch)) {
            break;
        }

        tty_input_push(ch);
        serial_rx_push(ch);
    }
}

static void _uart_irq_handler(u32 irq, void *ctx) {
    (void)irq;
    (void)ctx;
    _serial_drain_input();
}

static void _plic_init(void) {
    if (!boot_dtb) {
        log_info("PLIC init skipped: no boot DTB");
        return;
    }

    if (!plic_ready) {
        static const char *const plic_compat[] = {
            "sifive,plic-1.0.0",
            "riscv,plic0",
        };

        fdt_reg_t reg = {0};
        bool found = false;
        for (size_t i = 0; i < sizeof(plic_compat) / sizeof(plic_compat[0]); i++) {
            if (fdt_find_compatible_reg(boot_dtb, plic_compat[i], &reg) &&
                reg.addr && reg.size) {
                found = true;
                break;
            }
        }
        if (!found) {
            log_info("PLIC not described by boot DTB");
            return;
        }

        plic_phys = (uintptr_t)reg.addr;
        plic_span = (size_t)reg.size;
        plic_virt = (uintptr_t)arch_phys_map(plic_phys, plic_span, PHYS_MAP_MMIO);
        if (!plic_virt) {
            log_warn("failed to map RISC-V PLIC");
            return;
        }

        plic_ready = true;
        log_info(
            "PLIC mapped phys=%#lx span=%#zx virt=%#lx",
            (unsigned long)plic_phys,
            plic_span,
            (unsigned long)plic_virt
        );

        if (uart_irq) {
            if (irq_register(uart_irq, _uart_irq_handler, NULL)) {
                serial_set_rx_interrupt(uart_console_base(), true);
                log_info("UART IRQ %u registered on PLIC", (unsigned int)uart_irq);
            } else {
                log_warn("failed to register UART IRQ %u", (unsigned int)uart_irq);
            }
        } else {
            log_info("UART IRQ unavailable; polling only");
        }
    }

    size_t cpu_id = _current_cpu_id();
    _plic_init_context(cpu_id);
    log_info(
        "PLIC context ready cpu=%zu hart=%llu",
        cpu_id,
        (unsigned long long)_cpu_hartid(cpu_id)
    );
}

static bool _plic_handle_external(void) {
    if (!plic_ready) {
        return false;
    }

    size_t cpu_id = _current_cpu_id();
    u32 irq = *_plic_claim_reg(cpu_id);
    if (!irq) {
        return false;
    }

    u32 claim_cnt = __atomic_fetch_add(&ext_irq_claim_log_count, 1, __ATOMIC_RELAXED);
    if (claim_cnt < 8U) {
        log_info(
            "external irq claim=%u cpu=%zu hart=%llu",
            (unsigned int)irq,
            cpu_id,
            (unsigned long long)_cpu_hartid(cpu_id)
        );
    }

    if (irq < RISCV_PLIC_MAX_IRQS && irq_table[irq].handler) {
        irq_table[irq].handler(irq, irq_table[irq].ctx);
    } else {
        u32 cnt = __atomic_fetch_add(&ext_irq_log_count, 1, __ATOMIC_RELAXED);
        if (cnt < 8U) {
            log_warn("unhandled RISC-V external irq %u", (unsigned int)irq);
        }
    }

    *_plic_claim_reg(cpu_id) = irq;
    return true;
}

static void *_early_alloc(size_t size, size_t align) {
    if (!size) {
        return NULL;
    }

    size_t effective_align = align ? align : 16;
    uintptr_t cursor = ALIGN(early_cursor, effective_align);
    uintptr_t next = cursor + ALIGN(size, effective_align);

    if (next < cursor || next > early_limit) {
        panic("RISC-V early allocator exhausted");
    }

    early_cursor = next;
    memset((void *)cursor, 0, next - cursor);
    return (void *)cursor;
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
    *entry |= pte_leaf_flags(flags);
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

    for (size_t i = 0; i < mmio_region_count; i++) {
        mmio_region_t *region = &mmio_regions[i];
        u64 region_end = region->paddr + region->size;
        if (base >= region->paddr && end <= region_end) {
            return region->vaddr + (uintptr_t)(paddr - region->paddr);
        }
    }

    if (mmio_region_count >= RISCV_MMIO_MAX_REGIONS) {
        log_warn("RISC-V MMIO region table full");
        return 0;
    }

    uintptr_t vaddr = ALIGN(mmio_next_vaddr, PAGE_4KIB);
    size_t span = (size_t)(end - base);

    mmio_region_t *r = &mmio_regions[mmio_region_count++];
    r->paddr = base;
    r->size = span;
    r->vaddr = vaddr;

    mmio_next_vaddr = vaddr + span;
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

    for (size_t i = 0; i < mmio_region_count; i++) {
        mmio_region_t *region = &mmio_regions[i];
        u64 region_end = region->paddr + region->size;
        if (paddr >= region->paddr && end <= region_end) {
            return region->vaddr + (uintptr_t)(paddr - region->paddr);
        }
    }

    return 0;
}

static void _mmio_map_regions(page_t *root) {
    for (size_t i = 0; i < mmio_region_count; i++) {
        mmio_region_t *region = &mmio_regions[i];
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

    u64 now = riscv_read_time();
    u64 next = now + interval;
    u32 cnt = __atomic_fetch_add(&timer_arm_log_count, 1, __ATOMIC_RELAXED);

    if (cnt < 4U) {
        size_t cpu_id = _current_cpu_id();
        log_info(
            "timer arm cpu=%zu hart=%llu now=%llu next=%llu interval=%llu",
            cpu_id,
            (unsigned long long)_cpu_hartid(cpu_id),
            (unsigned long long)now,
            (unsigned long long)next,
            (unsigned long long)interval
        );
    }

    riscv_write_stimecmp(next);
}

static bool _handle_user_signal(int signum, arch_int_state_t *frame) {
    if (!frame || !arch_signal_is_user(frame) || !sched_is_running()) {
        return false;
    }

    sched_thread_t *thread = sched_current();
    if (!thread || !thread->user_thread) {
        return false;
    }

    sched_signal_send_thread(thread, signum);
    sched_signal_deliver_current(frame);
    return true;
}

static void _handle_page_fault(arch_int_state_t *frame, uintptr_t cause) {
    uintptr_t addr = frame ? frame->s_regs.stval : 0;
    bool write = cause == RISCV_EXC_STORE_PAGE;
    bool user = arch_signal_is_user(frame);

    if (write && user && sched_is_running()) {
        sched_thread_t *thread = sched_current();
        if (thread && thread->user_thread &&
            sched_handle_cow_fault(thread, addr, true)) {
            return;
        }
    }

    if (user) {
        _log_user_trap_once(
            &user_fault_log_count,
            8,
            "riscv user page fault",
            frame,
            cause
        );
    }

    if (_handle_user_signal(SIGSEGV, frame)) {
        return;
    }

    panic_prepare();
    log_fatal(
        "page fault cause=" RISCV_REG_FMT " addr=" RISCV_REG_FMT
        " sepc=" RISCV_REG_FMT,
        (unsigned long)cause,
        (unsigned long)addr,
        frame ? (unsigned long)frame->s_regs.sepc : 0UL
    );
    panic_dump_state(frame);
    panic_halt();
}

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

static void _register_boot_rootfs(void) {
    if (!boot_rootfs_paddr || !boot_rootfs_size) {
        log_info("no staged boot rootfs available");
        return;
    }

    if (boot_rootfs_registered) {
        return;
    }

    log_info(
        "register staged rootfs paddr=%#llx size=%zu KiB",
        (unsigned long long)boot_rootfs_paddr,
        boot_rootfs_size / 1024
    );

    boot_rootfs_t *rootfs = calloc(1, sizeof(*rootfs));
    disk_dev_t *disk = calloc(1, sizeof(*disk));

    if (!rootfs || !disk) {
        free(rootfs);
        free(disk);
        log_warn("failed to allocate staged rootfs disk");
        return;
    }

    rootfs->paddr = boot_rootfs_paddr;
    rootfs->size = boot_rootfs_size;

    disk->name = strdup("ram0");
    disk->type = DISK_VIRTUAL;
    disk->sector_size = BOOT_ROOTFS_SECTOR_SIZE;
    disk->sector_count = DIV_ROUND_UP(rootfs->size, (size_t)BOOT_ROOTFS_SECTOR_SIZE);
    disk->interface = &boot_rootfs_interface;
    disk->private = rootfs;

    if (!disk->name || !disk->sector_count || !disk_register(disk)) {
        free(disk->name);
        free(disk);
        free(rootfs);
        log_warn("failed to register staged rootfs");
        return;
    }

    boot_rootfs_registered = true;
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

    uintptr_t mem_end = (uintptr_t)(boot_memory_paddr + boot_memory_size);
    if (next <= dst || next > mem_end) {
        panic("boot DTB relocation exceeds RAM");
    }

    if (dst != src) {
        memmove((void *)dst, (const void *)src, dtb_size);
    }

    info->dtb_paddr = (u64)dst;
    info->dtb_size = (u64)dtb_size;
    boot_dtb = (const void *)dst;
    boot_dtb_size = dtb_size;

    if (!fdt_valid(boot_dtb)) {
        panic("relocated boot DTB is invalid");
    }

    log_info(
        "relocated DTB %#lx -> %#lx (%zu bytes)",
        (unsigned long)src,
        (unsigned long)dst,
        dtb_size
    );

    *reserved_end = next;
}

const kernel_args_t *arch_init(void *boot_info_ptr) {
    boot_info_t *info = boot_info_ptr;
    if (!info) {
        panic("boot info missing");
    }

    memcpy(&boot_args, &info->args, sizeof(boot_args));
    _configure_log_sinks(info);

    uart_phys = info->uart_paddr ? (uintptr_t)info->uart_paddr : SERIAL_UART0;

    cpu_hartid[0] = info->hartid;
    cpu_hartid_valid[0] = true;
    cpu_late_init[0] = true;

    boot_dtb = info->dtb_paddr ? (const void *)(uintptr_t)info->dtb_paddr : NULL;
    boot_dtb_size = info->dtb_size <= (u64)(size_t)-1 ? (size_t)info->dtb_size : 0;
    boot_memory_paddr = info->memory_paddr ? info->memory_paddr : RISCV_KERNEL_BASE;
    boot_memory_size = info->memory_size ? info->memory_size : (256ULL * MIB);
    boot_rootfs_paddr = info->boot_rootfs_paddr;
    boot_rootfs_size = info->boot_rootfs_size <= (u64)(size_t)-1
        ? (size_t)info->boot_rootfs_size : 0;

    uart_console_set_base(uart_phys);
    log_init(_log_puts);
    log_set_lvl(info->args.debug == DEBUG_ALL ? LOG_DEBUG : LOG_INFO);

    uart_irq = 0;
    if (boot_dtb) {
        (void)fdt_find_compatible_irq(boot_dtb, "ns16550a", &uart_irq);
    }
    if (!uart_irq && uart_phys == SERIAL_UART0) {
        uart_irq = RISCV_UART_DEFAULT_IRQ;
    }

    mmio_region_count = 0;
    mmio_next_vaddr = RISCV_MMIO_BASE;
    uart_virt = _mmio_register_region(uart_phys, RISCV_UART_WINDOW_SIZE);
    if (!uart_virt) {
        panic("failed to register UART MMIO window");
    }

#if __riscv_xlen == 64
    log_info("apheleiaOS kernel (riscv_64) booting");
#else
    log_info("apheleiaOS kernel (riscv_32) booting");
#endif
    _log_boot_info(info);
    log_info(
        "UART phys=%#lx virt=%#lx irq=%u",
        (unsigned long)uart_phys,
        (unsigned long)uart_virt,
        (unsigned int)uart_irq
    );

    if (boot_rootfs_paddr && boot_rootfs_size) {
        log_info(
            "staged rootfs at %#llx (%zu KiB)",
            (unsigned long long)boot_rootfs_paddr,
            boot_rootfs_size / 1024
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
    if (boot_rootfs_paddr && boot_rootfs_size) {
        uintptr_t rootfs_end = (uintptr_t)(boot_rootfs_paddr + boot_rootfs_size);
        if (rootfs_end > reserved_end) {
            reserved_end = rootfs_end;
        }
    }

    uintptr_t mem_end = (uintptr_t)(boot_memory_paddr + boot_memory_size);
    early_cursor = ALIGN(reserved_end, PAGE_4KIB);
    early_limit = mem_end;

    mmio_root = _early_alloc(PAGE_4KIB, PAGE_4KIB);
    _early_map_range(
        mmio_root,
        boot_memory_paddr,
        boot_memory_paddr,
        boot_memory_size,
        PT_WRITE | PT_GLOBAL
    );
    _mmio_map_regions(mmio_root);
    log_info(
        "early layout reserved_end=%#lx early=[%#lx,%#lx) mmio_root=%#lx mmio_regions=%zu",
        (unsigned long)reserved_end,
        (unsigned long)early_cursor,
        (unsigned long)early_limit,
        (unsigned long)(uintptr_t)mmio_root,
        mmio_region_count
    );

    trap_init();
    log_info("initialize kernel VM");
    vm_init_kernel(mmio_root);
    arch_vm_switch(arch_vm_kernel());
    log_info("switched to kernel VM");
    uart_console_set_base(uart_virt);
    _plic_init();

    log_info("initialize PMM");
    pmm_init(boot_memory_paddr, boot_memory_size, early_cursor);
    log_info("initialize heap");
    heap_init();
    log_info("initialize arch allocator");
    arch_init_alloc();
    log_info("initialize PMM refcounts");
    pmm_ref_init();
    if (!pmm_ref_ready()) {
        panic("PMM refcount table unavailable");
    }

    framebuffer_set_info(NULL);
    log_info("initialize UART console backend");
    uart_console_init(uart_virt);
    log_info("initialize console");
    console_init(info);
    log_console_ready = true;

    size_t text_cols = 0, text_rows = 0;
    if (console_get_size(&text_cols, &text_rows) && text_cols && text_rows) {
        uart_console_set_suppressed(true);
        arch_log_replay_console();
        uart_console_set_suppressed(false);
    }

    if (!driver_registry_init()) {
        log_warn("driver registry init failed");
    } else {
        log_info("load arch-early drivers");
        driver_load_stage(DRIVER_STAGE_ARCH_EARLY);
    }

    riscv_write_scounteren(
        RISCV_COUNTEREN_CY | RISCV_COUNTEREN_TM | RISCV_COUNTEREN_IR
    );
#if __riscv_xlen == 64
    log_info("apheleiaOS kernel (riscv_64) booted");
#else
    log_info("apheleiaOS kernel (riscv_32) booted");
#endif
    return &boot_args;
}

void arch_storage_init(void) {
    log_info("load storage drivers");
    driver_load_stage(DRIVER_STAGE_STORAGE);
    _register_boot_rootfs();
}

void arch_late_init(void) {
    size_t cpu_id = _current_cpu_id();
    if (!cpu_late_init[cpu_id]) {
        return;
    }

    log_info(
        "late init cpu=%zu hart=%llu plic=%s",
        cpu_id,
        (unsigned long long)_cpu_hartid(cpu_id),
        plic_ready ? "yes" : "no"
    );
    _timer_program_next();
    riscv_set_sie_bits(SIE_SSIE | SIE_STIE | (plic_ready ? SIE_SEIE : 0));
    cpu_late_init[cpu_id] = false;
    log_info("late init cpu=%zu complete", cpu_id);
}

void arch_log_replay_console(void) {
    unsigned long irq_flags = spin_lock_irqsave(&log_lock);
    _log_history_replay_console();
    spin_unlock_irqrestore(&log_lock, irq_flags);
}

void arch_smp_init(void) {
}

bool arch_supports_nx(void) {
    return true;
}

void *arch_phys_map(u64 paddr, size_t size, u32 flags) {
    if (paddr >= boot_memory_paddr &&
        paddr + size <= boot_memory_paddr + boot_memory_size) {
        return (void *)(uintptr_t)paddr;
    }

    uintptr_t mmio = _mmio_translate(paddr, size);
    if (mmio) {
        return (void *)mmio;
    }

    if ((flags & PHYS_MAP_MMIO) && size) {
        mmio = _mmio_register_region(paddr, size);
        if (mmio && mmio_root && mmio_region_count) {
            mmio_region_t *region = &mmio_regions[mmio_region_count - 1];
            map_region(
                mmio_root,
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
    stack_frame_t *frame = (stack_frame_t *)__builtin_frame_address(0);
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

void arch_dump_registers(const arch_int_state_t *frame) {
    if (!frame) {
        return;
    }

    log_fatal(
        "sepc=" RISCV_REG_FMT " sp=" RISCV_REG_FMT " sstatus=" RISCV_REG_FMT
        " scause=" RISCV_REG_FMT " stval=" RISCV_REG_FMT,
        (unsigned long)frame->s_regs.sepc,
        (unsigned long)frame->s_regs.sp,
        (unsigned long)frame->s_regs.sstatus,
        (unsigned long)frame->s_regs.scause,
        (unsigned long)frame->s_regs.stval
    );
}

void arch_tlb_flush(uintptr_t addr) {
    (void)addr;
    sfence_vma();
}

void arch_cpu_set_local(void *ptr) {
    cpu_local_ptr = (uintptr_t)ptr;
    riscv_write_tp((uintptr_t)ptr);
}

void *arch_cpu_get_local(void) {
    uintptr_t ptr = riscv_read_tp();
    return (void *)(ptr ? ptr : cpu_local_ptr);
}

unsigned long arch_irq_save(void) {
    unsigned long flags = riscv_read_sstatus();
    riscv_disable_irqs();
    return flags;
}

void arch_irq_restore(unsigned long flags) {
    unsigned long sstatus = riscv_read_sstatus() & ~SSTATUS_SIE;
    riscv_write_sstatus(sstatus | (flags & SSTATUS_SIE));
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
    trap_resched();
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
    return __atomic_load_n(&tick_count, __ATOMIC_ACQUIRE);
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
    syscall_handler = handler;
}

void arch_set_kernel_stack(uintptr_t sp) {
    kernel_sp = sp;
    riscv_write_sscratch(sp);
}

void arch_panic_enter(void) {
    if (uart_console_base()) {
        serial_set_rx_interrupt(uart_console_base(), false);
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

    unsigned long irq_flags = spin_lock_irqsave(&log_lock);
    size_t history_len = log_history_len;

    if (offset >= history_len) {
        spin_unlock_irqrestore(&log_lock, irq_flags);
        return 0;
    }

    size_t copy_len = history_len - offset;
    if (copy_len > len) {
        copy_len = len;
    }

    memcpy(buf, log_history + offset, copy_len);
    spin_unlock_irqrestore(&log_lock, irq_flags);
    return (ssize_t)copy_len;
}

size_t arch_log_ring_size(void) {
    unsigned long irq_flags = spin_lock_irqsave(&log_lock);
    size_t history_len = log_history_len;
    spin_unlock_irqrestore(&log_lock, irq_flags);
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

void trap_init(void) {
    if (((uintptr_t)trap_entry & 0x3U) != 0) {
        panic("RISC-V trap entry misaligned");
    }

    riscv_write_stvec((uintptr_t)trap_entry);
    log_info("trap vector stvec=%#lx", (unsigned long)(uintptr_t)trap_entry);
}

void trap_handle(arch_int_state_t *frame) {
    if (!frame) {
        panic("missing trap state");
    }

#if __riscv_xlen == 64
    bool interrupt = (frame->s_regs.scause >> 63) != 0;
    uintptr_t cause = frame->s_regs.scause & ~(1UL << 63);
#else
    bool interrupt = (frame->s_regs.scause >> 31) != 0;
    uintptr_t cause = frame->s_regs.scause & ~(1UL << 31);
#endif

    if (interrupt) {
        switch (cause) {
        case RISCV_IRQ_SOFT:
            _log_trap_once(
                &soft_irq_log_count,
                4,
                "RISC-V soft IRQ",
                frame,
                cause
            );
            riscv_clear_sip_bits(SIP_SSIP);
            sched_resched_softirq(frame);
            return;
        case RISCV_IRQ_TIMER:
            _log_trap_once(
                &timer_irq_log_count,
                2,
                "RISC-V timer IRQ",
                frame,
                cause
            );
            __atomic_add_fetch(&tick_count, 1, __ATOMIC_RELAXED);
            _timer_program_next();
            _serial_drain_input();
            sched_tick(frame);
            return;
        case RISCV_IRQ_EXTERNAL:
            _log_trap_once(
                &trap_log_count,
                8,
                "RISC-V external IRQ trap",
                frame,
                cause
            );
            _plic_handle_external();
            return;
        default:
            _log_trap_once(
                &trap_log_count,
                8,
                "RISC-V interrupt",
                frame,
                cause
            );
            return;
        }
    }

    if (cause != RISCV_EXC_U_ECALL) {
        _log_trap_once(&trap_log_count, 8, "RISC-V exception", frame, cause);
    }

    switch (cause) {
    case RISCV_EXC_BREAK:
        if (
            !arch_signal_is_user(frame) &&
            (uintptr_t)frame->s_regs.sepc == (uintptr_t)trap_resched
        ) {
            frame->s_regs.sepc += 4;
            sched_resched_softirq(frame);
            return;
        }
        break;
    case RISCV_EXC_U_ECALL:
        sched_capture_context(frame);
        frame->s_regs.sepc += 4;
        if (syscall_handler) {
            unsigned long irq_flags = arch_irq_save();

            riscv_enable_irqs(); // re-enable so blocking syscalls can be preempted

            syscall_handler(frame);
            arch_irq_restore(irq_flags);
        } else {
            frame->g_regs.a0 = (uintptr_t)-ENOSYS;
        }
        return;
    case RISCV_EXC_INST_PAGE:
    case RISCV_EXC_LOAD_PAGE:
    case RISCV_EXC_STORE_PAGE:
        _handle_page_fault(frame, cause);
        return;
    case RISCV_EXC_ILL:
        _log_user_trap_once(
            &user_ill_log_count,
            8,
            "riscv user illegal instruction",
            frame,
            cause
        );
        if (_handle_user_signal(SIGILL, frame)) {
            return;
        }
        panic_prepare();
        log_fatal(
            "illegal instruction at " RISCV_REG_FMT,
            (unsigned long)frame->s_regs.sepc
        );
        panic_dump_state(frame);
        panic_halt();
        return;
    default:
        panic_prepare();
        log_fatal(
            "unhandled trap cause=" RISCV_REG_FMT " sepc=" RISCV_REG_FMT
            " stval=" RISCV_REG_FMT,
            (unsigned long)cause,
            (unsigned long)frame->s_regs.sepc,
            (unsigned long)frame->s_regs.stval
        );
        panic_dump_state(frame);
        panic_halt();
        return;
    }
}

static bool irq_register(u32 irq, irq_handler_t handler, void *ctx) {
    if (!irq || irq >= RISCV_PLIC_MAX_IRQS || !handler) {
        return false;
    }

    unsigned long flags = arch_irq_save();
    irq_table[irq].handler = handler;
    irq_table[irq].ctx = ctx;
    _plic_toggle_irq(irq, true);
    arch_irq_restore(flags);
    return true;
}
