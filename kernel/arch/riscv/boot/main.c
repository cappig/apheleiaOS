#include <base/attributes.h>
#include <base/units.h>
#include <common/elf.h>
#include <common/ext2.h>
#include <lib/boot.h>
#include <log/log.h>
#include <parse/fdt.h>
#include <riscv/arch_paging.h>
#include <riscv/asm.h>
#include <riscv/boot.h>
#include <riscv/serial.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/config.h>

#include "memory.h"
#include "tty.h"

extern char __stack_top;
extern char __image_start;

#define RISCV_BOOT_PAGE_SIZE 4096UL
#ifndef RISCV_BOOT_SCRATCH_OFFSET
#define RISCV_BOOT_SCRATCH_OFFSET (48ULL * MIB)
#endif
#ifndef RISCV_BOOT_IMAGE_ROOTFS_OFFSET
#define RISCV_BOOT_IMAGE_ROOTFS_OFFSET (2ULL * MIB)
#endif

#define RISCV_TIMEBASE_HZ_DEFAULT 10000000ULL
#define RISCV_CLINT_DEFAULT_BASE  0x02000000ULL
#define RISCV_CLINT_MTIMECMP_OFF  0x00004000ULL
#define RISCV_CLINT_MTIME_OFF     0x0000BFF8ULL
#define RISCV_DTB_LOW_WINDOW_MAX  (16ULL * MIB)
#define RISCV_DTB_RAM_WINDOW_SIZE (256ULL * MIB)

#ifndef RISCV_BOOT_FORCE_NO_DTB
#define RISCV_BOOT_FORCE_NO_DTB 0
#endif

#ifndef RISCV_FRISC
#define RISCV_FRISC 0
#endif

#ifndef RISCV_BOOT_PLATFORM_DTB
#define RISCV_BOOT_PLATFORM_DTB "/boot/platform.dtb"
#endif

#ifndef RISCV_BOOT_IMAGE_BASE_OVERRIDE
#define RISCV_BOOT_IMAGE_BASE_OVERRIDE 0ULL
#endif
#ifndef BOOT_LOG_COLOR
#define BOOT_LOG_COLOR true
#endif

struct riscv_elf_load_ctx {
    const u8 *blob;
};

typedef struct {
    const u8 *image;
    size_t limit;
    bool from_initrd;
} boot_rootfs_src_t;

typedef struct {
    uintptr_t hartid;
    const void *dtb;
    size_t dtb_size;
    const u8 *rootfs_image;
    size_t rootfs_size;
    fdt_reg_t memory;
    uintptr_t uart;
} boot_desc_t;

uintptr_t riscv_boot_timer_cmp_addr = 0;
uintptr_t riscv_boot_timer_time_addr = 0;
u64 riscv_boot_timer_timebase_hz = RISCV_TIMEBASE_HZ_DEFAULT;
u8 riscv_boot_timer_ready = 0;

extern void mtrap_entry(void);
extern char riscv_boot_mtrap_scratch[];

static void boot_log_sink(const char *msg) {
    puts(msg);
}

static unsigned int boot_log_options(void) {
#if BOOT_LOG_COLOR
    return LOG_OPT_LOCATION | LOG_OPT_COLOR;
#else
    return LOG_OPT_LOCATION;
#endif
}

static void commit_boot_log(boot_info_t *info) {
    if (!info) {
        return;
    }

    size_t len = 0;
    size_t cap = 0;
    const char *buf = boot_log_buffer(&len, &cap);

    info->boot_log_paddr = (uintptr_t)buf;
    info->boot_log_len = len;
    info->boot_log_cap = cap;
}

static bool load_kernel_segment(const elf_segment_t *seg, void *ctx) {
    const struct riscv_elf_load_ctx *load_ctx = ctx;
    const u8 *blob = load_ctx->blob;

    u64 dst = seg->paddr ? seg->paddr : seg->vaddr;
    if (!dst) {
        log_debug(
            "skipping segment without load address off=%#llx filesz=%#llx memsz=%#llx flags=%#lx",
            (unsigned long long)seg->offset,
            (unsigned long long)seg->file_size,
            (unsigned long long)seg->mem_size,
            (unsigned long)seg->flags
        );

        return false;
    }

    log_debug(
        "loading segment off=%#llx dst=%#llx filesz=%#llx memsz=%#llx flags=%#lx",
        (unsigned long long)seg->offset,
        (unsigned long long)dst,
        (unsigned long long)seg->file_size,
        (unsigned long long)seg->mem_size,
        (unsigned long)seg->flags
    );

    memcpy((void *)(uintptr_t)dst, blob + seg->offset, seg->file_size);

    if (seg->mem_size > seg->file_size) {
        void *bss = (void *)(uintptr_t)(dst + seg->file_size);
        size_t bss_len = seg->mem_size - seg->file_size;

        log_debug("clearing segment bss addr=%#llx len=%#zx", (unsigned long long)(dst + seg->file_size), bss_len);

        memset(bss, 0, bss_len);
    }

    return true;
}

static bool load_kernel_elf(const u8 *blob, size_t blob_size, uintptr_t *out_entry) {
    struct riscv_elf_load_ctx ctx = { .blob = blob };
    elf_info_t info = { 0 };

    log_debug("parsing ELF blob=%#lx size=%zu", (unsigned long)(uintptr_t)blob, blob_size);

    if (!elf_foreach_segment(blob, blob_size, load_kernel_segment, &ctx, &info)) {
        log_debug("ELF parse failed");
        return false;
    }

    *out_entry = (uintptr_t)info.entry;
    log_debug("ELF entry=%#lx class=%s", (unsigned long)*out_entry, info.is_64 ? "elf64" : "elf32");

    return true;
}

static const void *sanitize_dtb_ptr(const void *dtb) {
    uintptr_t addr = (uintptr_t)dtb;

    if (!addr || (addr & 0x3U) != 0) {
        return NULL;
    }

    bool low_window = addr >= RISCV_BOOT_PAGE_SIZE && addr < RISCV_DTB_LOW_WINDOW_MAX;
    bool ram_window = addr >= RISCV_KERNEL_BASE && addr < (RISCV_KERNEL_BASE + RISCV_DTB_RAM_WINDOW_SIZE);

    if (!low_window && !ram_window) {
        return NULL;
    }

    return fdt_valid(dtb) ? dtb : NULL;
}

static bool detect_mtimer(
    const void *dtb,
    uintptr_t hartid,
    uintptr_t *mtime_addr_out,
    uintptr_t *mtimecmp_addr_out,
    u64 *timebase_hz_out,
    const char **kind_out
) {
    if (mtime_addr_out) {
        *mtime_addr_out = 0;
    }

    if (mtimecmp_addr_out) {
        *mtimecmp_addr_out = 0;
    }

    if (timebase_hz_out) {
        *timebase_hz_out = RISCV_TIMEBASE_HZ_DEFAULT;
    }

    if (kind_out) {
        *kind_out = "none";
    }

    u64 timebase_hz = RISCV_TIMEBASE_HZ_DEFAULT;
    if (dtb) {
        u64 parsed_hz = 0;
        if (fdt_find_timebase_frequency(dtb, &parsed_hz) && parsed_hz) {
            timebase_hz = parsed_hz;
        }
    }

    fdt_reg_t reg = { 0 };
    bool have_aclint = false;

    if (dtb && fdt_find_compatible_reg(dtb, "riscv,aclint-mtimer", &reg)) {
        u64 cmp_end = (u64)(hartid + 1U) * 8ULL + 8ULL;
        have_aclint = reg.addr != 0 && reg.size >= cmp_end;
    }

    if (have_aclint) {
        if (mtime_addr_out) {
            *mtime_addr_out = (uintptr_t)(reg.addr + reg.size - 8ULL);
        }

        if (mtimecmp_addr_out) {
            *mtimecmp_addr_out = (uintptr_t)(reg.addr + (u64)hartid * 8ULL);
        }

        if (timebase_hz_out) {
            *timebase_hz_out = timebase_hz;
        }

        if (kind_out) {
            *kind_out = "aclint-mtimer";
        }

        return true;
    }

    bool have_clint = false;

    if (dtb) {
        have_clint = fdt_find_compatible_reg(dtb, "sifive,clint0", &reg);

        if (!have_clint || !reg.addr) {
            have_clint = fdt_find_compatible_reg(dtb, "riscv,clint0", &reg);
        }

        have_clint = have_clint && reg.addr != 0;
    }

    if (have_clint) {
        if (mtime_addr_out) {
            *mtime_addr_out = (uintptr_t)(reg.addr + RISCV_CLINT_MTIME_OFF);
        }

        if (mtimecmp_addr_out) {
            *mtimecmp_addr_out = (uintptr_t)(reg.addr + RISCV_CLINT_MTIMECMP_OFF + (u64)hartid * 8ULL);
        }

        if (timebase_hz_out) {
            *timebase_hz_out = timebase_hz;
        }

        if (kind_out) {
            *kind_out = "clint";
        }

        return true;
    }

    bool use_spike_timer = !dtb;
    if (dtb) {
        use_spike_timer = fdt_has_compatible(dtb, "ucbbar,spike-bare-dev");
        use_spike_timer = use_spike_timer || fdt_has_compatible(dtb, "ucbbar,spike-bare");
    }

    if (use_spike_timer) {
        if (mtime_addr_out) {
            *mtime_addr_out = (uintptr_t)(RISCV_CLINT_DEFAULT_BASE + RISCV_CLINT_MTIME_OFF);
        }

        if (mtimecmp_addr_out) {
            *mtimecmp_addr_out = (uintptr_t)(RISCV_CLINT_DEFAULT_BASE + RISCV_CLINT_MTIMECMP_OFF + (u64)hartid * 8ULL);
        }

        if (timebase_hz_out) {
            *timebase_hz_out = timebase_hz;
        }

        if (kind_out) {
            *kind_out = "clint-default";
        }

        return true;
    }

    return false;
}

NORETURN static void enter_supervisor(uintptr_t entry, boot_info_t *info) {
    // delegate all standard exceptions to S-mode
    unsigned long medeleg = (1UL << 0) | (1UL << 1) | (1UL << 2) | (1UL << 3) | (1UL << 4) | (1UL << 5) | (1UL << 6) |
                            (1UL << 7) | (1UL << 8) | (1UL << 12) | (1UL << 13) | (1UL << 15);
    unsigned long mideleg = MIP_SSIP | MIP_STIP | (1UL << 9);
    unsigned long mstatus = riscv_read_mstatus();

    log_debug(
        "entering supervisor entry=%#lx info=%#lx hart=%llu",
        (unsigned long)entry,
        (unsigned long)(uintptr_t)info,
        (unsigned long long)(info ? info->hartid : 0)
    );
    log_debug("supervisor delegation medeleg=%#lx mideleg=%#lx mstatus=%#lx", medeleg, mideleg, mstatus);
    commit_boot_log(info);

    riscv_write_medeleg(medeleg);
    riscv_write_mideleg(mideleg);
    riscv_write_mcounteren(RISCV_COUNTEREN_CY | RISCV_COUNTEREN_TM | RISCV_COUNTEREN_IR);
    riscv_write_mtvec((uintptr_t)mtrap_entry);
    riscv_write_mscratch((uintptr_t)riscv_boot_mtrap_scratch);
    riscv_clear_mip_bits(MIP_STIP);
    riscv_write_mie(riscv_boot_timer_ready ? MIE_MTIE : 0);

    riscv_write_pmpaddr0((uintptr_t)-1);
    riscv_write_pmpcfg0(PMP_R | PMP_W | PMP_X | PMP_A_NAPOT);

    mstatus &= ~MSTATUS_MPP_MASK;
    mstatus |= MSTATUS_MPP_S;
    mstatus &= ~MSTATUS_MIE;
    mstatus |= MSTATUS_MPIE;

    riscv_write_mstatus(mstatus);

    riscv_write_mepc(entry);

    asm volatile("mv a0, %0\n"
                 "mv a1, %1\n"
                 "mret\n"
                 :
                 : "r"(info), "r"((uintptr_t)info->hartid)
                 : "memory");

    __builtin_unreachable();
}

static uintptr_t detect_uart_base(const void *dtb) {
    fdt_reg_t reg = { 0 };

    if (dtb && fdt_valid(dtb)) {
        if (fdt_find_compatible_reg(dtb, "ns16550a", &reg) && reg.addr) {
            return (uintptr_t)reg.addr;
        }

        if (fdt_has_compatible(dtb, "ucbbar,spike-bare-dev") || fdt_has_compatible(dtb, "ucbbar,spike-bare")) {
            // run-spike wires an NS16550-compatible MMIO plugin at UART0
            return SERIAL_UART0;
        }

        // valid DTB with no NS16550 means "no MMIO UART" on this platform
        return 0;
    }

    return SERIAL_UART0;
}

static uintptr_t detect_uart_stride(const void *dtb) {
    if (!dtb || !fdt_valid(dtb)) {
        return RISCV_UART_STRIDE;
    }

    u32 shift = 0;
    if (fdt_find_compatible_u32(dtb, "ns16550a", "reg-shift", &shift) && shift <= 3U) {
        return (uintptr_t)1 << shift;
    }

    return RISCV_UART_STRIDE;
}

static fdt_reg_t detect_memory(const void *dtb) {
    fdt_reg_t reg = {
        .addr = RISCV_KERNEL_BASE,
        .size = 256ULL * MIB,
    };

    fdt_reg_t probed = { 0 };
    if (dtb && fdt_find_memory_reg(dtb, &probed) && probed.addr && probed.size) {
        reg = probed;
    }

    return reg;
}

static void init_boot_args(kernel_args_t *args) {
    memset(args, 0, sizeof(*args));

    args->debug = BOOT_DEFAULT_DEBUG;
    args->stage_rootfs = 1;
    args->video = BOOT_DEFAULT_VIDEO;
    args->vesa_width = (u16)BOOT_DEFAULT_VESA_WIDTH;
    args->vesa_height = (u16)BOOT_DEFAULT_VESA_HEIGHT;
    args->vesa_bpp = BOOT_DEFAULT_VESA_BPP;
    strncpy(args->console, "/dev/ttyS0,/dev/console", sizeof(args->console) - 1);
    strncpy(args->font, BOOT_DEFAULT_FONT, sizeof(args->font) - 1);
}

static bool read_rootfs_image(void *dest, size_t offset, size_t bytes, void *ctx) {
    const u8 *image = (const u8 *)ctx;

    if (!dest || !image) {
        return false;
    }

    memcpy(dest, image + offset, bytes);
    return true;
}

static void
keep_out_of_heap(const char *name, uintptr_t start, size_t size, uintptr_t *heap_start, uintptr_t *heap_end) {
    if (!start || !size || !heap_start || !heap_end) {
        return;
    }

    if (*heap_start >= *heap_end) {
        return;
    }

    uintptr_t end = start + size;
    if (end <= start) {
        panic("boot reservation overflow");
    }

    if (end <= *heap_start || start >= *heap_end) {
        return;
    }

    uintptr_t low_end = ALIGN_DOWN(start, RISCV_BOOT_PAGE_SIZE);
    uintptr_t high_start = ALIGN(end, RISCV_BOOT_PAGE_SIZE);

    size_t low_size = low_end > *heap_start ? low_end - *heap_start : 0;
    size_t high_size = high_start < *heap_end ? *heap_end - high_start : 0;

    if (!low_size && !high_size) {
        panic("boot heap overlaps reserved image");
    }

    if (low_size >= high_size) {
        *heap_end = low_end;
    } else {
        *heap_start = high_start;
    }

    log_debug(
        "%s: kept out of boot heap [%#lx, %#lx)",
        name ? name : "reservation",
        (unsigned long)start,
        (unsigned long)end
    );
}

static bool dtb_initrd(const void *dtb, fdt_reg_t *reg) {
    if (!dtb || !fdt_find_initrd(dtb, reg)) {
        return false;
    }

    if (!reg->addr || !reg->size) {
        return false;
    }

    return reg->size <= (u64)(size_t)-1;
}

static void check_initrd_range(const u8 *image, size_t size, fdt_reg_t memory, uintptr_t memory_end) {
    uintptr_t start = (uintptr_t)image;
    uintptr_t end = start + size;

    bool wraps = end <= start;
    bool below_ram = start < (uintptr_t)memory.addr;
    bool above_ram = end > memory_end;

    if (wraps || below_ram || above_ram) {
        panic("initrd outside RAM");
    }
}

static void log_boot_hart(uintptr_t hartid, const void *dtb) {
    u64 boot_hart = 0;
    bool known = dtb && fdt_boot_cpuid_phys(dtb, &boot_hart);

    if (known) {
        log_debug("boot hart dtb=%llu current=%lu", (unsigned long long)boot_hart, (unsigned long)hartid);
    } else {
        log_debug("boot hart missing from DTB, defaulting to hart 0 (current=%lu)", (unsigned long)hartid);
    }
}

static bool secondary_hart(uintptr_t hartid, const void *dtb) {
    u64 boot_hart = 0;
    bool known = dtb && fdt_boot_cpuid_phys(dtb, &boot_hart);

    if (known) {
        return hartid != (uintptr_t)boot_hart;
    }

    return hartid != 0;
}

static void log_timer_status(const char *kind) {
    if (!riscv_boot_timer_ready) {
        log_warn("no machine timer backend detected");
        return;
    }

    u64 interval = riscv_boot_timer_timebase_hz / TIMER_FREQ;
    if (!interval) {
        interval = 1;
    }

    log_debug(
        "timer %s time=%#lx cmp=%#lx timebase=%lluHz interval=%llu",
        kind,
        (unsigned long)riscv_boot_timer_time_addr,
        (unsigned long)riscv_boot_timer_cmp_addr,
        (unsigned long long)riscv_boot_timer_timebase_hz,
        (unsigned long long)interval
    );
}

static void load_platform_dtb(boot_ext2_t *rootfs, const void **dtb, size_t *size) {
    if (!RISCV_FRISC) {
        return;
    }

    size_t file_size = 0;
    void *file = boot_ext2_read_file(rootfs, RISCV_BOOT_PLATFORM_DTB, &file_size);

    if (file && fdt_valid(file)) {
        *dtb = file;
        *size = fdt_size(file);

        log_debug("loaded DTB %s size=%zu", RISCV_BOOT_PLATFORM_DTB, *size);
        return;
    }

    if (file) {
        free(file);
    }

    log_warn("DTB %s unavailable", RISCV_BOOT_PLATFORM_DTB);
}

static void *copy_dtb(const void *dtb, size_t size) {
    if (!dtb || !size) {
        log_warn("no DTB available");
        return NULL;
    }

    void *copy = boot_alloc_aligned(size, RISCV_BOOT_PAGE_SIZE, false);
    if (!copy) {
        panic("failed to copy dtb");
    }

    memcpy(copy, dtb, size);

    log_debug("copied DTB %zu bytes to %#lx", size, (unsigned long)(uintptr_t)copy);

    return copy;
}

static boot_rootfs_src_t choose_rootfs(const void *dtb, const u8 *embedded, fdt_reg_t memory, uintptr_t memory_end) {
    boot_rootfs_src_t rootfs = {
        .image = embedded,
        .limit = 0,
        .from_initrd = false,
    };
    fdt_reg_t initrd = { 0 };

    if (!dtb_initrd(dtb, &initrd)) {
        return rootfs;
    }

    rootfs.image = (const u8 *)(uintptr_t)initrd.addr;
    rootfs.limit = (size_t)initrd.size;
    rootfs.from_initrd = true;

    check_initrd_range(rootfs.image, rootfs.limit, memory, memory_end);
    return rootfs;
}

static void init_rootfs(boot_ext2_t *rootfs, const u8 *image, size_t limit, bool from_initrd) {
    if (from_initrd) {
        log_debug("rootfs from DTB initrd at %#lx limit=%zu KiB", (unsigned long)(uintptr_t)image, limit / 1024);
    } else {
        log_debug("rootfs from embedded image at %#lx", (unsigned long)(uintptr_t)image);
    }

    if (!boot_ext2_init(rootfs, read_rootfs_image, (void *)image, limit)) {
        panic("storage device not available");
    }

    log_debug(
        "rootfs ready block_size=%lu blocks=%lu size=%zu KiB",
        (unsigned long)ext2_block_size(&rootfs->superblock),
        (unsigned long)rootfs->superblock.block_count,
        rootfs->size / 1024
    );
}

static const char *kernel_path(void) {
#if __riscv_xlen == 64
    return boot_kernel_path(true);
#else
    return boot_kernel_path(false);
#endif
}

static boot_info_t *make_boot_info(const boot_desc_t *desc) {
    boot_info_t *info = boot_alloc_aligned(sizeof(*info), 16, true);
    if (!info) {
        panic("failed to allocate boot info");
    }

    info->magic = BOOT_INFO_MAGIC;
    init_boot_args(&info->args);
    info->hartid = desc->hartid;
    info->dtb_paddr = (uintptr_t)desc->dtb;
    info->dtb_size = desc->dtb_size;
    info->boot_rootfs_paddr = (uintptr_t)desc->rootfs_image;
    info->boot_rootfs_size = desc->rootfs_size;
    info->memory_paddr = desc->memory.addr;
    info->memory_size = desc->memory.size;
    info->uart_paddr = desc->uart;
    commit_boot_log(info);

    log_info(
        "boot info hart=%llu uart=%#llx dtb=%#llx/%zu memory=%#llx+%#llx rootfs=%#llx+%zu",
        (unsigned long long)info->hartid,
        (unsigned long long)info->uart_paddr,
        (unsigned long long)info->dtb_paddr,
        desc->dtb_size,
        (unsigned long long)info->memory_paddr,
        (unsigned long long)info->memory_size,
        (unsigned long long)info->boot_rootfs_paddr,
        desc->rootfs_size
    );
    commit_boot_log(info);

    return info;
}

NORETURN void boot_main(uintptr_t hartid, const void *dtb) {
    if (RISCV_BOOT_FORCE_NO_DTB) {
        dtb = NULL;
    }

    boot_ext2_t rootfs = { 0 };
    const void *entry_dtb = sanitize_dtb_ptr(dtb);
    const void *boot_dtb = RISCV_FRISC ? NULL : entry_dtb;
    bool dtb_valid = entry_dtb != NULL;
    size_t dtb_size = boot_dtb ? fdt_size(boot_dtb) : 0;

    uintptr_t image_base = RISCV_BOOT_IMAGE_BASE_OVERRIDE ? (uintptr_t)RISCV_BOOT_IMAGE_BASE_OVERRIDE
                                                          : (uintptr_t)&__image_start;

    const u8 *embedded_rootfs = (const u8 *)(image_base + RISCV_BOOT_IMAGE_ROOTFS_OFFSET);

    fdt_reg_t memory_reg = detect_memory(boot_dtb);
    uintptr_t uart_base = detect_uart_base(boot_dtb);
    uintptr_t uart_stride = detect_uart_stride(boot_dtb);
    uintptr_t heap_start = (uintptr_t)&__stack_top;
    uintptr_t memory_end = (uintptr_t)(memory_reg.addr + memory_reg.size);
    uintptr_t scratch_base = (uintptr_t)(memory_reg.addr + RISCV_BOOT_SCRATCH_OFFSET);

    serial_set_reg_stride(uart_stride);
    tty_set_uart_base(uart_base);
    log_init(boot_log_sink);
    log_set_lvl(LOG_DEBUG);
    log_set_options(boot_log_options());

    log_info(
        "entered boot stub hart=%lu dtb=%#lx valid=%s active=%s size=%zu",
        (unsigned long)hartid,
        (unsigned long)(uintptr_t)dtb,
        dtb_valid ? "yes" : "no",
        boot_dtb ? "yes" : "no",
        dtb_size
    );
    log_debug(
        "memory range base=%#llx size=%#llx end=%#lx",
        (unsigned long long)memory_reg.addr,
        (unsigned long long)memory_reg.size,
        (unsigned long)memory_end
    );
    log_info(
        "image layout image=%#lx rootfs=%#lx stack_top=%#lx scratch=%#lx uart=%#lx stride=%lu",
        (unsigned long)image_base,
        (unsigned long)(uintptr_t)embedded_rootfs,
        (unsigned long)(uintptr_t)&__stack_top,
        (unsigned long)scratch_base,
        (unsigned long)uart_base,
        (unsigned long)uart_stride
    );

    if (scratch_base > heap_start) {
        heap_start = scratch_base;
    }

    boot_rootfs_src_t rootfs_src = choose_rootfs(boot_dtb, embedded_rootfs, memory_reg, memory_end);

    log_boot_hart(hartid, boot_dtb);

    if (secondary_hart(hartid, boot_dtb)) {
        log_debug("parking secondary hart %lu", (unsigned long)hartid);
        halt();
    }

    const char *timer_kind = "none";
    riscv_boot_timer_ready = (u8)detect_mtimer(
        boot_dtb,
        hartid,
        &riscv_boot_timer_time_addr,
        &riscv_boot_timer_cmp_addr,
        &riscv_boot_timer_timebase_hz,
        &timer_kind
    );
    log_timer_status(timer_kind);

    heap_start = ALIGN(heap_start, RISCV_BOOT_PAGE_SIZE);
    memory_end = ALIGN_DOWN(memory_end, RISCV_BOOT_PAGE_SIZE);

    if (rootfs_src.from_initrd) {
        keep_out_of_heap("initrd", (uintptr_t)rootfs_src.image, rootfs_src.limit, &heap_start, &memory_end);
    }

    if (heap_start >= memory_end) {
        panic("no boot heap space available");
    }

    log_debug(
        "heap: [%#lx, %#lx) %lu KiB",
        (unsigned long)heap_start,
        (unsigned long)memory_end,
        (unsigned long)((memory_end - heap_start) / 1024UL)
    );

    boot_heap_init(heap_start, memory_end);

    init_rootfs(&rootfs, rootfs_src.image, rootfs_src.limit, rootfs_src.from_initrd);

    load_platform_dtb(&rootfs, &boot_dtb, &dtb_size);

    const char *path = kernel_path();
    log_debug("reading kernel %s", path);

    size_t kernel_size = 0;
    void *kernel_blob = boot_ext2_read_file(&rootfs, path, &kernel_size);
    if (!kernel_blob) {
        panic("no kernel image found");
    }

    log_debug("loaded kernel at %#lx size=%zu KiB", (unsigned long)(uintptr_t)kernel_blob, kernel_size / 1024);

    uintptr_t elf_entry = 0;
    if (!load_kernel_elf(kernel_blob, kernel_size, &elf_entry)) {
        panic("failed to load kernel ELF");
    }

    void *dtb_copy = copy_dtb(boot_dtb, dtb_size);
    boot_desc_t desc = {
        .hartid = hartid,
        .dtb = dtb_copy,
        .dtb_size = dtb_size,
        .rootfs_image = rootfs_src.image,
        .rootfs_size = rootfs.size,
        .memory = memory_reg,
        .uart = uart_base,
    };

    boot_info_t *info = make_boot_info(&desc);

    enter_supervisor(elf_entry, info);
}
