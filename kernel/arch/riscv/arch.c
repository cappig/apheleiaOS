#include <arch/arch.h>
#include <arch/paging.h>
#include <arch/signal.h>
#include <arch/thread.h>
#include <base/units.h>
#include <drivers/manager.h>
#include <errno.h>
#include <fs/ext2.h>
#include <inttypes.h>
#include <log/log.h>
#include <riscv/asm.h>
#include <riscv/boot.h>
#include <riscv/console.h>
#include <riscv/mm/heap.h>
#include <riscv/mm/physical.h>
#include <riscv/mm/virtual.h>
#include <riscv/serial.h>
#include <riscv/trap.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/console.h>
#include <sys/cpu.h>
#include <sys/disk.h>
#include <sys/framebuffer.h>
#include <sys/panic.h>
#include <sys/tty_input.h>

#define BOOT_ROOTFS_SECTOR_SIZE 512
#define RISCV_TIMEBASE_HZ       10000000ULL
#define RISCV_UART_WINDOW_SIZE  PAGE_4KIB
#define RISCV_IRQ_SOFT          1
#define RISCV_IRQ_TIMER         5
#define RISCV_EXC_ILL           2
#define RISCV_EXC_BREAK         3
#define RISCV_EXC_U_ECALL       8
#define RISCV_EXC_INST_PAGE     12
#define RISCV_EXC_LOAD_PAGE     13
#define RISCV_EXC_STORE_PAGE    15

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

static kernel_args_t boot_args = {0};
static arch_syscall_handler_t syscall_handler = NULL;

static page_t *kernel_root = NULL;
static uintptr_t early_alloc_cursor = 0;
static uintptr_t early_alloc_limit = 0;
static uintptr_t uart_phys_base = SERIAL_UART0;
static uintptr_t uart_phys_page = SERIAL_UART0;
static uintptr_t uart_virt_base = RISCV_MMIO_BASE;
static uintptr_t uart_io_base = SERIAL_UART0;
static u64 memory_paddr = RISCV_KERNEL_BASE;
static u64 memory_size = 0;
static u64 boot_rootfs_paddr = 0;
static size_t boot_rootfs_size = 0;
static bool boot_rootfs_registered = false;
static bool irq_late_ready = false;
static volatile u64 tick_count = 0;
static u32 user_irq_log_count = 0;
static u32 user_syscall_log_count = 0;
static u32 user_fault_log_count = 0;
static u32 user_ill_log_count = 0;
static u32 trap_log_count = 0;

extern char __bss_end;
extern char __stack_top;
extern void riscv_trap_entry(void);

void riscv_vm_init_kernel(page_t *root);

static void _log_puts(const char *s) {
    if (!s) {
        return;
    }

    send_serial_string(riscv_console_uart_base(), s);
}

static void
_log_user_trap_once(
    u32 *counter,
    u32 limit,
    const char *label,
    arch_int_state_t *state,
    u64 cause
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
        "%s pid=%ld cause=%#" PRIx64 " sepc=%#" PRIx64
        " sp=%#" PRIx64 " stval=%#" PRIx64
        " a7=%#" PRIx64 " a0=%#" PRIx64 " a1=%#" PRIx64,
        label,
        thread ? (long)thread->pid : 0L,
        cause,
        (u64)state->s_regs.sepc,
        (u64)state->s_regs.sp,
        (u64)state->s_regs.stval,
        (u64)state->g_regs.a7,
        (u64)state->g_regs.a0,
        (u64)state->g_regs.a1
    );
}

static void
_log_trap_once(
    u32 *counter,
    u32 limit,
    const char *label,
    arch_int_state_t *state,
    u64 cause,
    bool interrupt
) {
    if (!counter || !label || !state) {
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
        "%s pid=%ld irq=%d cause=%#" PRIx64 " sepc=%#" PRIx64
        " sp=%#" PRIx64 " stval=%#" PRIx64 " sstatus=%#" PRIx64
        " user=%d",
        label,
        thread ? (long)thread->pid : 0L,
        interrupt ? 1 : 0,
        cause,
        (u64)state->s_regs.sepc,
        (u64)state->s_regs.sp,
        (u64)state->s_regs.stval,
        (u64)state->s_regs.sstatus,
        arch_signal_is_user(state) ? 1 : 0
    );
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

    uintptr_t cursor = _align_up(early_alloc_cursor, align ? align : 16);
    uintptr_t next = cursor + ALIGN(size, align ? align : 16);

    if (next < cursor || next > early_alloc_limit) {
        panic("RISC-V early allocator exhausted");
    }

    early_alloc_cursor = next;
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

static void _timer_program_next(void) {
    u64 interval = RISCV_TIMEBASE_HZ / (TIMER_FREQ ? TIMER_FREQ : 1U);
    if (!interval) {
        interval = 1;
    }

    _sbi_set_timer(riscv_read_time() + interval);
}

static void _serial_drain_input(void) {
    for (size_t i = 0; i < 64; i++) {
        char ch = 0;
        if (!serial_try_receive(riscv_console_uart_base(), &ch)) {
            break;
        }

        tty_input_push(ch);
    }
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

static void _handle_page_fault(arch_int_state_t *state, u64 cause) {
    uintptr_t addr = state ? state->s_regs.stval : 0;
    bool write = cause == RISCV_EXC_STORE_PAGE;
    bool user = arch_signal_is_user(state);

    if (user) {
        _log_user_trap_once(
            &user_fault_log_count,
            8,
            "riscv user page fault",
            state,
            cause
        );
    }

    if (write && user && sched_is_running()) {
        sched_thread_t *thread = sched_current();
        if (thread && thread->user_thread &&
            sched_handle_cow_fault(thread, addr, true)) {
            return;
        }
    }

    if (_handle_user_signal(SIGSEGV, state)) {
        return;
    }

    panic_prepare();
    log_fatal(
        "page fault cause=%#" PRIx64 " addr=%#" PRIx64 " sepc=%#" PRIx64,
        cause,
        (u64)addr,
        state ? (u64)state->s_regs.sepc : 0
    );
    panic_dump_state(state);
    panic_halt();
}

static void _register_boot_rootfs(void) {
    if (!boot_rootfs_paddr || !boot_rootfs_size || boot_rootfs_registered) {
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

    rootfs->paddr = boot_rootfs_paddr;
    rootfs->size = boot_rootfs_size;

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

    boot_rootfs_registered = true;
    log_info(
        "registered /dev/%s from boot image (%zu KiB)",
        disk->name,
        rootfs->size / 1024
    );
}

const kernel_args_t *arch_init(void *boot_info_ptr) {
    boot_info_t *info = boot_info_ptr;

    if (!info) {
        panic("boot info missing");
    }

    send_serial_string(
        info->uart_paddr ? (uintptr_t)info->uart_paddr : SERIAL_UART0,
        "riscv: arch_init\n\r"
    );

    memcpy(&boot_args, &info->args, sizeof(boot_args));

    uart_phys_base = info->uart_paddr ? (uintptr_t)info->uart_paddr : SERIAL_UART0;
    uart_phys_page = ALIGN_DOWN(uart_phys_base, PAGE_4KIB);
    uart_virt_base = RISCV_MMIO_BASE + (uart_phys_base - uart_phys_page);
    uart_io_base = uart_virt_base;
    riscv_console_set_uart_base(uart_phys_base);

    log_init(_log_puts);
    log_set_lvl(info->args.debug == DEBUG_ALL ? LOG_DEBUG : LOG_INFO);
    send_serial_string(uart_phys_base, "riscv: arch_init/log\n\r");

    memory_paddr = info->memory_paddr ? info->memory_paddr : RISCV_KERNEL_BASE;
    memory_size = info->memory_size ? info->memory_size : (256ULL * MIB);
    boot_rootfs_paddr = info->boot_rootfs_paddr;
    boot_rootfs_size = 0;
    if (info->boot_rootfs_size <= (u64)(size_t)-1) {
        boot_rootfs_size = (size_t)info->boot_rootfs_size;
    }

    log_info(
        "riscv boot memory paddr=%#" PRIx64 " size=%#" PRIx64,
        memory_paddr,
        memory_size
    );

    cpu_init_boot();
    arch_cpu_set_local(&cores_local[0]);
    arch_set_kernel_stack((uintptr_t)&__stack_top);
    riscv_write_sstatus(riscv_read_sstatus() | SSTATUS_SUM);

    uintptr_t reserved_end = (uintptr_t)&__bss_end;
    uintptr_t info_end = (uintptr_t)info + sizeof(*info);

    if (info_end > reserved_end) {
        reserved_end = info_end;
    }

    if (info->dtb_paddr && info->dtb_size) {
        uintptr_t dtb_end = (uintptr_t)(info->dtb_paddr + info->dtb_size);
        if (dtb_end > reserved_end) {
            reserved_end = dtb_end;
        }
    }
    if (boot_rootfs_paddr && boot_rootfs_size) {
        uintptr_t rootfs_end = (uintptr_t)(boot_rootfs_paddr + boot_rootfs_size);
        if (rootfs_end > reserved_end) {
            reserved_end = rootfs_end;
        }
    }

    early_alloc_cursor = ALIGN(reserved_end, PAGE_4KIB);
    early_alloc_limit = (uintptr_t)(memory_paddr + memory_size);

    kernel_root = _early_alloc(PAGE_4KIB, PAGE_4KIB);
    _early_map_range(
        kernel_root,
        memory_paddr,
        memory_paddr,
        memory_size,
        PT_WRITE | PT_GLOBAL
    );
    _early_map_range(
        kernel_root,
        RISCV_MMIO_BASE,
        uart_phys_page,
        RISCV_UART_WINDOW_SIZE,
        PT_WRITE | PT_GLOBAL | PT_NO_EXECUTE
    );
    send_serial_string(uart_phys_base, "riscv: arch_init/map\n\r");

    riscv_trap_init();
    riscv_vm_init_kernel(kernel_root);
    arch_vm_switch(arch_vm_kernel());
    riscv_console_set_uart_base(uart_io_base);
    send_serial_string(uart_io_base, "riscv: arch_init/vm\n\r");

    pmm_init(memory_paddr, memory_size, early_alloc_cursor);
    heap_init();
    arch_init_alloc();
    pmm_ref_init();
    if (!pmm_ref_ready()) {
        panic("PMM refcount table unavailable");
    }
    send_serial_string(uart_io_base, "riscv: arch_init/mm\n\r");

    framebuffer_set_info(NULL);
    riscv_console_backend_init(uart_io_base);
    console_init(info);

    if (!driver_registry_init()) {
        log_warn("driver registry init failed");
    } else {
        driver_load_stage(DRIVER_STAGE_ARCH_EARLY);
    }
    send_serial_string(uart_io_base, "riscv: arch_init/dev\n\r");

    riscv_write_scounteren(
        RISCV_COUNTEREN_CY | RISCV_COUNTEREN_TM | RISCV_COUNTEREN_IR
    );
    irq_late_ready = true;

    log_info("apheleiaOS kernel (%s) booted", arch_name());
    return &boot_args;
}

void arch_storage_init(void) {
    _register_boot_rootfs();
}

void arch_late_init(void) {
    if (!irq_late_ready) {
        return;
    }

    send_serial_string(uart_io_base, "riscv: late_init/timer-set\n\r");
    _timer_program_next();
    send_serial_string(uart_io_base, "riscv: late_init/timer-ok\n\r");
    riscv_set_sie_bits(SIE_SSIE | SIE_STIE);
    send_serial_string(uart_io_base, "riscv: late_init/armed\n\r");
    irq_late_ready = false;
}

void arch_log_replay_console(void) {
}

void arch_smp_init(void) {
}

bool arch_supports_nx(void) {
    return true;
}

void *arch_phys_map(u64 paddr, size_t size, u32 flags) {
    (void)flags;

    if (paddr >= memory_paddr && paddr + size <= memory_paddr + memory_size) {
        return (void *)(uintptr_t)paddr;
    }

    if (
        paddr >= uart_phys_page &&
        paddr + size <= uart_phys_page + RISCV_UART_WINDOW_SIZE
    ) {
        return (void *)(uintptr_t)(RISCV_MMIO_BASE + (paddr - uart_phys_page));
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
}

void arch_dump_registers(const arch_int_state_t *state) {
    if (!state) {
        return;
    }

    log_fatal(
        "sepc=%#" PRIx64 " sp=%#" PRIx64 " sstatus=%#" PRIx64
        " scause=%#" PRIx64 " stval=%#" PRIx64,
        (u64)state->s_regs.sepc,
        (u64)state->s_regs.sp,
        (u64)state->s_regs.sstatus,
        (u64)state->s_regs.scause,
        (u64)state->s_regs.stval
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
    riscv_kernel_sp = sp;
    riscv_write_sscratch(sp);
}

void arch_panic_enter(void) {
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
    (void)buf;
    (void)offset;
    (void)len;
    return 0;
}

size_t arch_log_ring_size(void) {
    return 0;
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
    u64 cause = state->s_regs.scause & ~(1ULL << 63);
#else
    bool interrupt = (state->s_regs.scause >> 31) != 0;
    u64 cause = state->s_regs.scause & ~(1ULL << 31);
#endif

    _log_trap_once(
        &trap_log_count,
        16,
        "riscv trap",
        state,
        cause,
        interrupt
    );

    if (interrupt) {
        switch (cause) {
        case RISCV_IRQ_SOFT:
            riscv_clear_sip_bits(SIP_SSIP);
            sched_resched_softirq(state);
            return;
        case RISCV_IRQ_TIMER:
            __atomic_add_fetch(&tick_count, 1, __ATOMIC_RELAXED);
            _timer_program_next();
            _serial_drain_input();
            _log_user_trap_once(
                &user_irq_log_count,
                8,
                "riscv user timer irq",
                state,
                cause
            );
            sched_tick(state);
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
        _log_user_trap_once(
            &user_syscall_log_count,
            16,
            "riscv user ecall",
            state,
            cause
        );
        sched_capture_context(state);
        state->s_regs.sepc += 4;
        if (syscall_handler) {
            unsigned long irq_flags = arch_irq_save();

            // The generic scheduler expects blocking syscalls to be
            // preemptible while they wait in kernel context.
            riscv_enable_irqs();
            syscall_handler(state);
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
            &user_ill_log_count,
            8,
            "riscv user illegal instruction",
            state,
            cause
        );
        if (_handle_user_signal(SIGILL, state)) {
            return;
        }
        panic_prepare();
        log_fatal("illegal instruction at %#" PRIx64, (u64)state->s_regs.sepc);
        panic_dump_state(state);
        panic_halt();
        return;
    default:
        panic_prepare();
        log_fatal(
            "unhandled trap cause=%#" PRIx64 " sepc=%#" PRIx64 " stval=%#" PRIx64,
            cause,
            (u64)state->s_regs.sepc,
            (u64)state->s_regs.stval
        );
        panic_dump_state(state);
        panic_halt();
        return;
    }
}
