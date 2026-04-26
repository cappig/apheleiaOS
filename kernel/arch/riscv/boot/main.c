#include <base/units.h>
#include <base/attributes.h>
#include <riscv/arch_paging.h>
#include <riscv/asm.h>
#include <riscv/boot.h>
#include <riscv/serial.h>
#include <common/elf.h>
#include <lib/boot.h>
#include <parse/fdt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <common/ext2.h>

#include "memory.h"
#include "tty.h"

extern char __stack_top;
extern char __image_start;

#define RISCV_BOOT_PAGE_SIZE     4096UL
#define RISCV_BOOT_SCRATCH_OFFSET (32ULL * MIB)
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

#ifndef RISCV_BOOT_IMAGE_BASE_OVERRIDE
#define RISCV_BOOT_IMAGE_BASE_OVERRIDE 0ULL
#endif

#ifndef TIMER_FREQ
#define TIMER_FREQ 1000U
#endif

struct riscv_elf_load_ctx {
    const u8 *blob;
};

uintptr_t riscv_boot_timer_cmp_addr = 0;
uintptr_t riscv_boot_timer_time_addr = 0;
u64 riscv_boot_timer_timebase_hz = RISCV_TIMEBASE_HZ_DEFAULT;
u8 riscv_boot_timer_ready = 0;

extern void mtrap_entry(void);
extern char riscv_boot_mtrap_scratch[];

static void boot_logf(const char *fmt, ...) {
    char buf[PRINTF_BUF_SIZE];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    printf("[riscv boot] %s\n", buf);
}

static bool load_kernel_segment(const elf_segment_t *seg, void *ctx) {
    const struct riscv_elf_load_ctx *load_ctx = ctx;
    const u8 *blob = load_ctx->blob;

    u64 dst = seg->paddr ? seg->paddr : seg->vaddr;
    if (!dst) {
        boot_logf(
            "segment skip: no load address off=%#llx filesz=%#llx memsz=%#llx flags=%#x",
            (unsigned long long)seg->offset,
            (unsigned long long)seg->file_size,
            (unsigned long long)seg->mem_size,
            seg->flags
        );
        return false;
    }

    boot_logf(
        "segment load: off=%#llx dst=%#llx filesz=%#llx memsz=%#llx flags=%#x",
        (unsigned long long)seg->offset,
        (unsigned long long)dst,
        (unsigned long long)seg->file_size,
        (unsigned long long)seg->mem_size,
        seg->flags
    );

    memcpy((void *)(uintptr_t)dst, blob + seg->offset, seg->file_size);

    if (seg->mem_size > seg->file_size) {
        void *bss = (void *)(uintptr_t)(dst + seg->file_size);
        size_t bss_len = seg->mem_size - seg->file_size;
        boot_logf(
            "segment bss:  addr=%#llx len=%#zx",
            (unsigned long long)(dst + seg->file_size), bss_len
        );
        memset(bss, 0, bss_len);
    }

    return true;
}

static bool load_kernel_elf(const u8 *blob, size_t blob_size, uintptr_t *out_entry) {
    struct riscv_elf_load_ctx ctx = { .blob = blob };
    elf_info_t info = {0};

    boot_logf("elf parse: blob=%#lx size=%zu", (unsigned long)(uintptr_t)blob, blob_size);

    if (!elf_foreach_segment(blob, blob_size, load_kernel_segment, &ctx, &info)) {
        boot_logf("elf parse: failed");
        return false;
    }

    *out_entry = (uintptr_t)info.entry;
    boot_logf(
        "elf parse: entry=%#lx class=%s",
        (unsigned long)*out_entry, info.is_64 ? "elf64" : "elf32"
    );
    return true;
}

static const void *sanitize_dtb_ptr(const void *dtb) {
    uintptr_t addr = (uintptr_t)dtb;

    if (!addr || (addr & 0x3U) != 0) {
        return NULL;
    }

    bool low_window = addr >= RISCV_BOOT_PAGE_SIZE && addr < RISCV_DTB_LOW_WINDOW_MAX;
    bool ram_window =
        addr >= RISCV_KERNEL_BASE &&
        addr < (RISCV_KERNEL_BASE + RISCV_DTB_RAM_WINDOW_SIZE);

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

    fdt_reg_t reg = {0};
    if (
        dtb &&
        fdt_find_compatible_reg(dtb, "riscv,aclint-mtimer", &reg) &&
        reg.addr &&
        reg.size >= ((u64)(hartid + 1U) * 8ULL + 8ULL)
    ) {
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

    if (
        dtb &&
        (
            (fdt_find_compatible_reg(dtb, "sifive,clint0", &reg) && reg.addr) ||
            (fdt_find_compatible_reg(dtb, "riscv,clint0", &reg) && reg.addr)
        )
    ) {
        if (mtime_addr_out) {
            *mtime_addr_out = (uintptr_t)(reg.addr + RISCV_CLINT_MTIME_OFF);
        }
        if (mtimecmp_addr_out) {
            *mtimecmp_addr_out = (uintptr_t)(
                reg.addr + RISCV_CLINT_MTIMECMP_OFF + (u64)hartid * 8ULL
            );
        }
        if (timebase_hz_out) {
            *timebase_hz_out = timebase_hz;
        }
        if (kind_out) {
            *kind_out = "clint";
        }
        return true;
    }

    if (
        !dtb ||
        fdt_has_compatible(dtb, "ucbbar,spike-bare-dev") ||
        fdt_has_compatible(dtb, "ucbbar,spike-bare")
    ) {
        if (mtime_addr_out) {
            *mtime_addr_out = (uintptr_t)(RISCV_CLINT_DEFAULT_BASE + RISCV_CLINT_MTIME_OFF);
        }
        if (mtimecmp_addr_out) {
            *mtimecmp_addr_out = (uintptr_t)(
                RISCV_CLINT_DEFAULT_BASE + RISCV_CLINT_MTIMECMP_OFF +
                (u64)hartid * 8ULL
            );
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
    unsigned long medeleg =
        (1UL << 0) | (1UL << 1) | (1UL << 2) | (1UL << 3) |
        (1UL << 4) | (1UL << 5) | (1UL << 6) | (1UL << 7) |
        (1UL << 8) | (1UL << 12) | (1UL << 13) | (1UL << 15);
    unsigned long mideleg = MIP_SSIP | MIP_STIP | (1UL << 9);
    unsigned long mstatus = riscv_read_mstatus();

    boot_logf(
        "supervisor: entry=%#lx info=%#lx hart=%llu",
        (unsigned long)entry,
        (unsigned long)(uintptr_t)info,
        (unsigned long long)(info ? info->hartid : 0)
    );
    boot_logf(
        "supervisor: medeleg=%#lx mideleg=%#lx mstatus=%#lx",
        medeleg, mideleg, mstatus
    );

    riscv_write_medeleg(medeleg);
    riscv_write_mideleg(mideleg);
    riscv_write_mcounteren(
        RISCV_COUNTEREN_CY | RISCV_COUNTEREN_TM | RISCV_COUNTEREN_IR
    );
    riscv_write_mtvec((uintptr_t)mtrap_entry);
    riscv_write_mscratch((uintptr_t)riscv_boot_mtrap_scratch);
    riscv_clear_mip_bits(MIP_STIP);
    riscv_write_mie(riscv_boot_timer_ready ? MIE_MTIE : 0);

    riscv_write_pmpaddr0((uintptr_t)-1);
    riscv_write_pmpcfg0(PMP_R | PMP_W | PMP_X | PMP_A_NAPOT);

    mstatus &= ~MSTATUS_MPP_MASK;
    mstatus |= MSTATUS_MPP_S;
    mstatus &= ~(MSTATUS_MIE | MSTATUS_MPIE);

    riscv_write_mstatus(mstatus);

    riscv_write_mepc(entry);

    asm volatile(
        "mv a0, %0\n"
        "mv a1, %1\n"
        "mret\n"
        :
        : "r"(info), "r"((uintptr_t)info->hartid)
        : "memory"
    );

    __builtin_unreachable();
}

static uintptr_t detect_uart_base(const void *dtb) {
    fdt_reg_t reg = {0};

    if (dtb && fdt_valid(dtb)) {
        if (fdt_find_compatible_reg(dtb, "ns16550a", &reg) && reg.addr) {
            return (uintptr_t)reg.addr;
        }

        if (fdt_has_compatible(dtb, "ucbbar,spike-bare-dev") ||
            fdt_has_compatible(dtb, "ucbbar,spike-bare")) {
            // run-spike wires an NS16550-compatible MMIO plugin at UART0
            return SERIAL_UART0;
        }

        // valid DTB with no NS16550 means "no MMIO UART" on this platform
        return 0;
    }

    return SERIAL_UART0;
}

static fdt_reg_t detect_memory(const void *dtb) {
    fdt_reg_t reg = {
        .addr = RISCV_KERNEL_BASE,
        .size = 256ULL * MIB,
    };

    fdt_reg_t probed = {0};
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

NORETURN void boot_main(uintptr_t hartid, const void *dtb) {
    if (RISCV_BOOT_FORCE_NO_DTB) {
        dtb = NULL;
    }

    boot_ext2_t rootfs = {0};
    const void *boot_dtb = sanitize_dtb_ptr(dtb);
    bool dtb_valid = boot_dtb != NULL;
    size_t dtb_size = boot_dtb ? fdt_size(boot_dtb) : 0;
    fdt_reg_t initrd_reg = {0};

    uintptr_t image_base = RISCV_BOOT_IMAGE_BASE_OVERRIDE ?
        (uintptr_t)RISCV_BOOT_IMAGE_BASE_OVERRIDE :
        (uintptr_t)&__image_start;
    const u8 *embedded_rootfs =
        (const u8 *)(image_base + RISCV_BOOT_IMAGE_ROOTFS_OFFSET);
    const u8 *rootfs_image = embedded_rootfs;
    size_t rootfs_limit = 0;
    bool rootfs_from_initrd = false;

    fdt_reg_t memory_reg = detect_memory(boot_dtb);
    uintptr_t uart_base = detect_uart_base(boot_dtb);
    uintptr_t heap_start = (uintptr_t)&__stack_top;
    uintptr_t memory_end = (uintptr_t)(memory_reg.addr + memory_reg.size);
    uintptr_t scratch_base = (uintptr_t)(memory_reg.addr + RISCV_BOOT_SCRATCH_OFFSET);

    tty_set_uart_base(uart_base);

    boot_logf(
        "entry: hart=%lu dtb=%#lx valid=%s size=%zu",
        (unsigned long)hartid,
        (unsigned long)(uintptr_t)dtb,
        dtb_valid ? "yes" : "no",
        dtb_size
    );
    boot_logf(
        "memory: base=%#llx size=%#llx end=%#lx",
        (unsigned long long)memory_reg.addr,
        (unsigned long long)memory_reg.size,
        (unsigned long)memory_end
    );
    boot_logf(
        "layout: image=%#lx rootfs=%#lx stack_top=%#lx scratch=%#lx uart=%#lx",
        (unsigned long)image_base,
        (unsigned long)(uintptr_t)embedded_rootfs,
        (unsigned long)(uintptr_t)&__stack_top,
        (unsigned long)scratch_base,
        (unsigned long)uart_base
    );

    if (scratch_base > heap_start) {
        heap_start = scratch_base;
    }

    if (boot_dtb && fdt_find_initrd(boot_dtb, &initrd_reg) &&
        initrd_reg.addr &&
        initrd_reg.size &&
        initrd_reg.size <= (u64)(size_t)-1) {
        rootfs_image = (const u8 *)(uintptr_t)initrd_reg.addr;
        rootfs_limit = (size_t)initrd_reg.size;
        rootfs_from_initrd = true;
    }

    u64 boot_hart = 0;
    bool boot_hart_known = boot_dtb && fdt_boot_cpuid_phys(boot_dtb, &boot_hart);

    if (boot_hart_known) {
        boot_logf(
            "boot hart: dtb says %llu, we are %lu",
            (unsigned long long)boot_hart, (unsigned long)hartid
        );
    } else {
        boot_logf(
            "boot hart: not in DTB, defaulting to hart 0 (we are %lu)",
            (unsigned long)hartid
        );
    }

    if ((boot_hart_known && hartid != (uintptr_t)boot_hart) ||
        (!boot_hart_known && hartid != 0)) {
        boot_logf("parking secondary hart %lu", (unsigned long)hartid);
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

    if (riscv_boot_timer_ready) {
        u64 interval = riscv_boot_timer_timebase_hz / (TIMER_FREQ ? TIMER_FREQ : 1U);
        if (!interval) {
            interval = 1;
        }

        boot_logf(
            "timer: %s time=%#lx cmp=%#lx timebase=%lluHz interval=%llu",
            timer_kind,
            (unsigned long)riscv_boot_timer_time_addr,
            (unsigned long)riscv_boot_timer_cmp_addr,
            (unsigned long long)riscv_boot_timer_timebase_hz,
            (unsigned long long)interval
        );
    } else {
        boot_logf("timer: no machine timer backend detected");
    }

    heap_start = ALIGN(heap_start, RISCV_BOOT_PAGE_SIZE);
    memory_end = ALIGN_DOWN(memory_end, RISCV_BOOT_PAGE_SIZE);
    if (heap_start >= memory_end) {
        panic("no boot heap space available");
    }

    boot_logf(
        "heap: [%#lx, %#lx) %lu KiB",
        (unsigned long)heap_start,
        (unsigned long)memory_end,
        (unsigned long)((memory_end - heap_start) / 1024UL)
    );

    boot_heap_init(heap_start, memory_end);

    if (rootfs_from_initrd) {
        boot_logf(
            "rootfs: init from DTB initrd at %#lx limit=%zu KiB",
            (unsigned long)(uintptr_t)rootfs_image,
            rootfs_limit / 1024
        );
    } else {
        boot_logf(
            "rootfs: init from embedded image at %#lx",
            (unsigned long)(uintptr_t)rootfs_image
        );
    }

    if (!boot_ext2_init(&rootfs, read_rootfs_image, (void *)rootfs_image, rootfs_limit)) {
        panic("storage device not available");
    }
    boot_logf(
        "rootfs: ready block_size=%u blocks=%u size=%zu KiB",
        ext2_block_size(&rootfs.superblock),
        rootfs.superblock.block_count,
        rootfs.size / 1024
    );

#if __riscv_xlen == 64
    const char *kernel_path = boot_kernel_path(true);
#else
    const char *kernel_path = boot_kernel_path(false);
#endif

    boot_logf("kernel: reading %s", kernel_path);

    size_t kernel_size = 0;
    void *kernel_blob = boot_ext2_read_file(&rootfs, kernel_path, &kernel_size);
    if (!kernel_blob) {
        panic("no kernel image found");
    }

    boot_logf(
        "kernel: loaded at %#lx size=%zu KiB",
        (unsigned long)(uintptr_t)kernel_blob, kernel_size / 1024
    );

    uintptr_t elf_entry = 0;
    if (!load_kernel_elf(kernel_blob, kernel_size, &elf_entry)) {
        panic("failed to load kernel ELF");
    }

    void *dtb_copy = NULL;
    if (boot_dtb && dtb_size) {
        dtb_copy = boot_alloc_aligned(dtb_size, RISCV_BOOT_PAGE_SIZE, false);
        if (!dtb_copy) {
            panic("failed to copy dtb");
        }
        memcpy(dtb_copy, boot_dtb, dtb_size);
        boot_logf(
            "dtb: copied %zu bytes to %#lx",
            dtb_size, (unsigned long)(uintptr_t)dtb_copy
        );
    } else {
        boot_logf("dtb: none available");
    }

    boot_info_t *info = boot_alloc_aligned(sizeof(*info), 16, true);
    if (!info) {
        panic("failed to allocate boot info");
    }

    info->magic = BOOT_INFO_MAGIC;
    init_boot_args(&info->args);
    info->hartid = hartid;
    info->dtb_paddr = (uintptr_t)dtb_copy;
    info->dtb_size = dtb_size;
    info->boot_rootfs_paddr = (uintptr_t)rootfs_image;
    info->boot_rootfs_size = rootfs.size;
    info->memory_paddr = memory_reg.addr;
    info->memory_size = memory_reg.size;
    info->uart_paddr = uart_base;

    boot_logf(
        "boot_info: hart=%llu uart=%#llx dtb=%#llx/%zu memory=%#llx+%#llx rootfs=%#llx+%zu",
        (unsigned long long)info->hartid,
        (unsigned long long)info->uart_paddr,
        (unsigned long long)info->dtb_paddr,
        dtb_size,
        (unsigned long long)info->memory_paddr,
        (unsigned long long)info->memory_size,
        (unsigned long long)info->boot_rootfs_paddr,
        rootfs.size
    );

    enter_supervisor(elf_entry, info);
}
