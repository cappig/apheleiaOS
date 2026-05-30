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
#include "mtrap.h"
#include "tty.h"

extern char __stack_top;
extern char __image_start;

#define BOOT_PAGE_SIZE 4096UL
#ifndef BOOT_SCRATCH_OFFSET
#define BOOT_SCRATCH_OFFSET (48ULL * MIB)
#endif
#ifndef BOOT_ROOTFS_OFFSET
#define BOOT_ROOTFS_OFFSET (2ULL * MIB)
#endif

#define DEFAULT_TIMEBASE_HZ 10000000ULL
#define CLINT_BASE          0x02000000ULL
#define MTIMECMP_OFFSET     0x00004000ULL
#define MTIME_OFFSET        0x0000BFF8ULL
#define DTB_LOW_LIMIT       (16ULL * MIB)
#define DTB_RAM_WINDOW      (256ULL * MIB)

#ifndef RISCV_FRISC
#define RISCV_FRISC 0
#endif

#ifndef BOOT_DTB_PATH
#define BOOT_DTB_PATH "/boot/platform.dtb"
#endif

#ifndef BOOT_LOG_COLOR
#define BOOT_LOG_COLOR true
#endif

typedef struct {
    const u8 *blob;
} elf_load_t;

typedef struct {
    const u8 *image;
    size_t limit;
    bool from_initrd;
} rootfs_src_t;

typedef struct {
    uintptr_t hartid;
    const void *dtb;
    size_t dtb_size;
    const u8 *rootfs_image;
    size_t rootfs_size;
    fdt_reg_t memory;
    uintptr_t uart;
} boot_desc_t;

typedef struct {
    uintptr_t image_base;
    const u8 *embedded_rootfs;
    uintptr_t heap_start;
    uintptr_t memory_end;
    uintptr_t scratch_base;
} layout_t;

typedef struct {
    uintptr_t start;
    uintptr_t end;
} range_t;

typedef struct {
    uintptr_t time;
    uintptr_t cmp;
    u64 hz;
    bool ready;
} boot_timer_t;

boot_timer_t boot_timer = {
    .hz = DEFAULT_TIMEBASE_HZ,
};

static void log_sink(const char *msg) {
    puts(msg);
}

static const char *yesno(bool value) {
    if (value) {
        return "yes";
    }

    return "no";
}

static const char *elf_class(const elf_info_t *info) {
    if (!info) {
        return "unknown";
    }

    if (info->is_64) {
        return "elf64";
    }

    return "elf32";
}

static unsigned int log_opts(void) {
#if BOOT_LOG_COLOR
    return LOG_OPT_LOCATION | LOG_OPT_COLOR;
#else
    return LOG_OPT_LOCATION;
#endif
}

static void commit_log(boot_info_t *info) {
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

static bool load_seg(const elf_segment_t *seg, void *ctx) {
    const elf_load_t *load_ctx = ctx;
    const u8 *blob = load_ctx->blob;

    u64 dst = seg->paddr;
    if (!dst) {
        dst = seg->vaddr;
    }

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
        "ELF segment off=%#llx dst=%#llx filesz=%#llx memsz=%#llx flags=%#lx",
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

        log_debug("ELF bss addr=%#llx len=%#zx", (unsigned long long)(dst + seg->file_size), bss_len);

        memset(bss, 0, bss_len);
    }

    return true;
}

static bool load_elf(const u8 *blob, size_t blob_size, uintptr_t *out_entry) {
    elf_load_t ctx = { .blob = blob };
    elf_info_t info = { 0 };

    log_debug("parsing kernel ELF blob=%#lx size=%zu", (unsigned long)(uintptr_t)blob, blob_size);

    if (!elf_foreach_segment(blob, blob_size, load_seg, &ctx, &info)) {
        log_debug("kernel ELF parse failed");
        return false;
    }

    *out_entry = (uintptr_t)info.entry;
    log_debug("kernel entry=%#lx class=%s", (unsigned long)*out_entry, elf_class(&info));

    return true;
}

// accept only pointers that look like firmware dtbs in low memory or ram
static const void *valid_dtb(const void *dtb) {
    uintptr_t addr = (uintptr_t)dtb;

    if (!addr || (addr & 0x3U) != 0) {
        return NULL;
    }

    bool low_window = addr >= BOOT_PAGE_SIZE && addr < DTB_LOW_LIMIT;
    bool ram_window = addr >= RISCV_KERNEL_BASE && addr < (RISCV_KERNEL_BASE + DTB_RAM_WINDOW);

    if (!low_window && !ram_window) {
        return NULL;
    }

    if (!fdt_valid(dtb)) {
        return NULL;
    }

    return dtb;
}

static uintptr_t addr_add(uintptr_t base, u64 offset, const char *what) {
    if (offset > (u64)((uintptr_t)-1 - base)) {
        const char *reason = what;
        if (!reason) {
            reason = "boot address overflow";
        }

        panic(reason);
    }

    return base + (uintptr_t)offset;
}

static const u8 *rootfs_at(uintptr_t image_base) {
    uintptr_t rootfs = addr_add(image_base, BOOT_ROOTFS_OFFSET, "embedded rootfs address overflow");

    return (const u8 *)rootfs;
}

static layout_t make_layout(fdt_reg_t memory) {
    layout_t layout = {
        .image_base = (uintptr_t)&__image_start,
        .heap_start = (uintptr_t)&__stack_top,
    };

    layout.embedded_rootfs = rootfs_at(layout.image_base);
    layout.memory_end = addr_add((uintptr_t)memory.addr, memory.size, "memory range overflow");
    layout.scratch_base = addr_add((uintptr_t)memory.addr, BOOT_SCRATCH_OFFSET, "scratch address overflow");

    if (layout.scratch_base > layout.heap_start) {
        layout.heap_start = layout.scratch_base;
    }

    return layout;
}

static bool find_mtimer(
    const void *dtb,
    uintptr_t hartid,
    uintptr_t *time_out,
    uintptr_t *cmp_out,
    u64 *hz_out,
    const char **kind
) {
    if (time_out) {
        *time_out = 0;
    }

    if (cmp_out) {
        *cmp_out = 0;
    }

    if (hz_out) {
        *hz_out = DEFAULT_TIMEBASE_HZ;
    }

    if (kind) {
        *kind = "none";
    }

    u64 hz = DEFAULT_TIMEBASE_HZ;
    if (dtb) {
        u64 dtb_hz = 0;
        if (fdt_find_timebase_frequency(dtb, &dtb_hz) && dtb_hz) {
            hz = dtb_hz;
        }
    }

    fdt_reg_t reg = { 0 };
    bool aclint = false;

    if (dtb && fdt_find_compatible_reg(dtb, "riscv,aclint-mtimer", &reg)) {
        u64 cmp_end = (u64)(hartid + 1U) * 8ULL + 8ULL;
        aclint = reg.addr != 0 && reg.size >= cmp_end;
    }

    if (aclint) {
        if (time_out) {
            *time_out = (uintptr_t)(reg.addr + reg.size - 8ULL);
        }

        if (cmp_out) {
            *cmp_out = (uintptr_t)(reg.addr + (u64)hartid * 8ULL);
        }

        if (hz_out) {
            *hz_out = hz;
        }

        if (kind) {
            *kind = "aclint-mtimer";
        }

        return true;
    }

    bool clint = false;

    if (dtb) {
        clint = fdt_find_compatible_reg(dtb, "sifive,clint0", &reg);

        if (!clint || !reg.addr) {
            clint = fdt_find_compatible_reg(dtb, "riscv,clint0", &reg);
        }

        clint = clint && reg.addr != 0;
    }

    if (clint) {
        if (time_out) {
            *time_out = (uintptr_t)(reg.addr + MTIME_OFFSET);
        }

        if (cmp_out) {
            *cmp_out = (uintptr_t)(reg.addr + MTIMECMP_OFFSET + (u64)hartid * 8ULL);
        }

        if (hz_out) {
            *hz_out = hz;
        }

        if (kind) {
            *kind = "clint";
        }

        return true;
    }

    bool spike = !dtb;
    if (dtb) {
        spike = fdt_has_compatible(dtb, "ucbbar,spike-bare-dev");
        spike = spike || fdt_has_compatible(dtb, "ucbbar,spike-bare");
    }

    if (spike) {
        if (time_out) {
            *time_out = (uintptr_t)(CLINT_BASE + MTIME_OFFSET);
        }

        if (cmp_out) {
            *cmp_out = (uintptr_t)(CLINT_BASE + MTIMECMP_OFFSET + (u64)hartid * 8ULL);
        }

        if (hz_out) {
            *hz_out = hz;
        }

        if (kind) {
            *kind = "clint-default";
        }

        return true;
    }

    return false;
}

// all boot state is committed before mret because s-mode owns the console next
static NORETURN void enter_supervisor(uintptr_t entry, boot_info_t *info) {
    unsigned long medeleg = (1UL << 0) | (1UL << 1) | (1UL << 2) | (1UL << 3) | (1UL << 4) | (1UL << 5) | (1UL << 6) |
                            (1UL << 7) | (1UL << 8) | (1UL << 12) | (1UL << 13) | (1UL << 15);

    unsigned long mideleg = MIP_SSIP | MIP_STIP | (1UL << 9);
    unsigned long mstatus = riscv_read_mstatus();

    u64 hartid = 0;

    if (info) {
        hartid = info->hartid;
    }

    log_debug(
        "entering supervisor entry=%#lx info=%#lx hart=%llu",
        (unsigned long)entry,
        (unsigned long)(uintptr_t)info,
        (unsigned long long)hartid
    );
    log_debug("delegation medeleg=%#lx mideleg=%#lx mstatus=%#lx", medeleg, mideleg, mstatus);
    commit_log(info);

    riscv_write_medeleg(medeleg);
    riscv_write_mideleg(mideleg);

    riscv_write_mcounteren(RISCV_COUNTEREN_CY | RISCV_COUNTEREN_TM | RISCV_COUNTEREN_IR);

    riscv_write_mtvec((uintptr_t)mtrap_entry);
    riscv_write_mscratch((uintptr_t)trap_scratch);

    riscv_clear_mip_bits(MIP_STIP);

    if (boot_timer.ready) {
        riscv_write_mie(MIE_MTIE);
    } else {
        riscv_write_mie(0);
    }

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

static uintptr_t find_uart(const void *dtb) {
    fdt_reg_t reg = { 0 };

    if (dtb && fdt_valid(dtb)) {
        if (fdt_find_compatible_reg(dtb, "ns16550a", &reg) && reg.addr) {
            return (uintptr_t)reg.addr;
        }

        if (fdt_has_compatible(dtb, "ucbbar,spike-bare-dev") || fdt_has_compatible(dtb, "ucbbar,spike-bare")) {
            return SERIAL_UART0;
        }

        return 0;
    }

    return SERIAL_UART0;
}

static uintptr_t find_stride(const void *dtb) {
    if (!dtb || !fdt_valid(dtb)) {
        return RISCV_UART_STRIDE;
    }

    u32 shift = 0;
    if (fdt_find_compatible_u32(dtb, "ns16550a", "reg-shift", &shift) && shift <= 3U) {
        return (uintptr_t)1 << shift;
    }

    return RISCV_UART_STRIDE;
}

static fdt_reg_t find_memory(const void *dtb) {
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

static void init_args(kernel_args_t *args) {
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

static bool read_image(void *dest, size_t offset, size_t bytes, void *ctx) {
    const u8 *image = (const u8 *)ctx;

    if (!dest || !image) {
        return false;
    }

    memcpy(dest, image + offset, bytes);
    return true;
}

static void reserve(const char *name, uintptr_t start, size_t size, range_t *heap) {
    if (!start || !size || !heap) {
        return;
    }

    if (heap->start >= heap->end) {
        return;
    }

    uintptr_t end = start + size;
    if (end <= start) {
        panic("boot reservation overflow");
    }

    if (end <= heap->start || start >= heap->end) {
        return;
    }

    uintptr_t low_end = ALIGN_DOWN(start, BOOT_PAGE_SIZE);
    uintptr_t high_start = ALIGN(end, BOOT_PAGE_SIZE);

    size_t low_size = 0;
    size_t high_size = 0;

    const char *log_name = "reservation";

    if (low_end > heap->start) {
        low_size = low_end - heap->start;
    }

    if (high_start < heap->end) {
        high_size = heap->end - high_start;
    }

    if (name) {
        log_name = name;
    }

    if (!low_size && !high_size) {
        panic("boot heap overlaps reserved image");
    }

    if (low_size >= high_size) {
        heap->end = low_end;
    } else {
        heap->start = high_start;
    }

    log_debug("%s kept out of boot heap [%#lx, %#lx)", log_name, (unsigned long)start, (unsigned long)end);
}

static bool find_initrd(const void *dtb, fdt_reg_t *reg) {
    if (!dtb || !fdt_find_initrd(dtb, reg)) {
        return false;
    }

    if (!reg->addr || !reg->size) {
        return false;
    }

    return reg->size <= (u64)(size_t)-1;
}

static void check_initrd(const u8 *image, size_t size, fdt_reg_t memory, uintptr_t memory_end) {
    uintptr_t start = (uintptr_t)image;
    uintptr_t end = start + size;

    bool wraps = end <= start;
    bool below_ram = start < (uintptr_t)memory.addr;
    bool above_ram = end > memory_end;

    if (wraps || below_ram || above_ram) {
        panic("initrd outside RAM");
    }
}

static void log_hart(uintptr_t hartid, const void *dtb) {
    u64 boot_hart = 0;
    bool known = dtb && fdt_boot_cpuid_phys(dtb, &boot_hart);

    if (known) {
        log_debug("boot hart dtb=%llu current=%lu", (unsigned long long)boot_hart, (unsigned long)hartid);
    } else {
        log_debug("boot hart missing; using hart 0 (current=%lu)", (unsigned long)hartid);
    }
}

static bool is_secondary(uintptr_t hartid, const void *dtb) {
    u64 boot_hart = 0;
    bool known = dtb && fdt_boot_cpuid_phys(dtb, &boot_hart);

    if (known) {
        return hartid != (uintptr_t)boot_hart;
    }

    return hartid != 0;
}

static void log_timer(const char *kind) {
    if (!boot_timer.ready) {
        log_warn("machine timer unavailable");
        return;
    }

    u64 interval = boot_timer.hz / TIMER_FREQ;
    if (!interval) {
        interval = 1;
    }

    log_debug(
        "timer %s time=%#lx cmp=%#lx timebase=%llu Hz interval=%llu",
        kind,
        (unsigned long)boot_timer.time,
        (unsigned long)boot_timer.cmp,
        (unsigned long long)boot_timer.hz,
        (unsigned long long)interval
    );
}

static void load_dtb(boot_ext2_t *rootfs, const void **dtb, size_t *size) {
    if (!RISCV_FRISC) {
        return;
    }

    size_t file_size = 0;
    void *file = boot_ext2_read_file(rootfs, BOOT_DTB_PATH, &file_size);

    if (file && fdt_valid(file)) {
        *dtb = file;
        *size = fdt_size(file);

        log_debug("loaded DTB %s size=%zu", BOOT_DTB_PATH, *size);
        return;
    }

    if (file) {
        free(file);
    }

    log_warn("DTB %s unavailable", BOOT_DTB_PATH);
}

static void *copy_dtb(const void *dtb, size_t size) {
    if (!dtb || !size) {
        log_warn("no DTB available");
        return NULL;
    }

    void *copy = boot_alloc_aligned(size, BOOT_PAGE_SIZE, false);
    if (!copy) {
        panic("failed to copy DTB");
    }

    memcpy(copy, dtb, size);

    log_debug("copied DTB size=%zu dst=%#lx", size, (unsigned long)(uintptr_t)copy);

    return copy;
}

static rootfs_src_t pick_rootfs(const void *dtb, const u8 *embedded, fdt_reg_t memory, uintptr_t memory_end) {
    rootfs_src_t rootfs = {
        .image = embedded,
        .limit = 0,
        .from_initrd = false,
    };

    fdt_reg_t initrd = { 0 };

    if (!find_initrd(dtb, &initrd)) {
        return rootfs;
    }

    rootfs.image = (const u8 *)(uintptr_t)initrd.addr;
    rootfs.limit = (size_t)initrd.size;
    rootfs.from_initrd = true;

    check_initrd(rootfs.image, rootfs.limit, memory, memory_end);
    return rootfs;
}

static void mount_rootfs(boot_ext2_t *rootfs, const u8 *image, size_t limit, bool from_initrd) {
    if (from_initrd) {
        log_debug("rootfs from DTB initrd at %#lx limit=%zu KiB", (unsigned long)(uintptr_t)image, limit / 1024);
    } else {
        log_debug("rootfs from embedded image at %#lx", (unsigned long)(uintptr_t)image);
    }

    if (!boot_ext2_init(rootfs, read_image, (void *)image, limit)) {
        panic("storage device not available");
    }

    log_debug(
        "rootfs ready block=%lu blocks=%lu size=%zu KiB",
        (unsigned long)ext2_block_size(&rootfs->superblock),
        (unsigned long)rootfs->superblock.block_count,
        rootfs->size / 1024
    );
}

static const char *kernel_file(void) {
#if __riscv_xlen == 64
    return boot_kernel_path(true);
#else
    return boot_kernel_path(false);
#endif
}

static boot_info_t *make_info(const boot_desc_t *desc) {
    boot_info_t *info = boot_alloc_aligned(sizeof(*info), 16, true);
    if (!info) {
        panic("failed to allocate boot info");
    }

    info->magic = BOOT_INFO_MAGIC;
    init_args(&info->args);

    info->hartid = desc->hartid;
    info->dtb_paddr = (uintptr_t)desc->dtb;
    info->dtb_size = desc->dtb_size;

    info->boot_rootfs_paddr = (uintptr_t)desc->rootfs_image;
    info->boot_rootfs_size = desc->rootfs_size;

    info->memory_paddr = desc->memory.addr;
    info->memory_size = desc->memory.size;
    info->uart_paddr = desc->uart;

    commit_log(info);

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
    commit_log(info);

    return info;
}

static const void *pick_dtb(const void *entry_dtb) {
    if (RISCV_FRISC) {
        return NULL;
    }

    return entry_dtb;
}

static size_t dtb_len(const void *dtb) {
    if (!dtb) {
        return 0;
    }

    return fdt_size(dtb);
}

static const char *setup_timer(const void *dtb, uintptr_t hartid) {
    const char *kind = "none";

    boot_timer.ready = find_mtimer(dtb, hartid, &boot_timer.time, &boot_timer.cmp, &boot_timer.hz, &kind);

    return kind;
}

NORETURN void boot_main(uintptr_t hartid, const void *dtb) {
    boot_ext2_t rootfs = { 0 };

    const void *entry_dtb = valid_dtb(dtb);
    const void *boot_dtb = pick_dtb(entry_dtb);

    bool dtb_valid = entry_dtb != NULL;
    size_t dtb_size = dtb_len(boot_dtb);

    fdt_reg_t memory = find_memory(boot_dtb);
    layout_t layout = make_layout(memory);

    uintptr_t uart = find_uart(boot_dtb);
    uintptr_t stride = find_stride(boot_dtb);

    serial_set_reg_stride(stride);
    tty_set_uart_base(uart);

    log_init(log_sink);
    log_set_lvl(LOG_DEBUG);
    log_set_options(log_opts());

    log_info(
        "boot stub hart=%lu dtb=%#lx valid=%s active=%s size=%zu",
        (unsigned long)hartid,
        (unsigned long)(uintptr_t)dtb,
        yesno(dtb_valid),
        yesno(boot_dtb != NULL),
        dtb_size
    );
    log_debug(
        "memory base=%#llx size=%#llx end=%#lx",
        (unsigned long long)memory.addr,
        (unsigned long long)memory.size,
        (unsigned long)layout.memory_end
    );
    log_info(
        "layout image=%#lx rootfs=%#lx stack=%#lx scratch=%#lx uart=%#lx stride=%lu",
        (unsigned long)layout.image_base,
        (unsigned long)(uintptr_t)layout.embedded_rootfs,
        (unsigned long)(uintptr_t)&__stack_top,
        (unsigned long)layout.scratch_base,
        (unsigned long)uart,
        (unsigned long)stride
    );

    rootfs_src_t rootfs_src = pick_rootfs(boot_dtb, layout.embedded_rootfs, memory, layout.memory_end);

    log_hart(hartid, boot_dtb);

    if (is_secondary(hartid, boot_dtb)) {
        log_debug("parking secondary hart %lu", (unsigned long)hartid);
        halt();
    }

    const char *timer_kind = setup_timer(boot_dtb, hartid);
    log_timer(timer_kind);

    range_t heap = {
        .start = ALIGN(layout.heap_start, BOOT_PAGE_SIZE),
        .end = ALIGN_DOWN(layout.memory_end, BOOT_PAGE_SIZE),
    };

    if (rootfs_src.from_initrd) {
        reserve("initrd", (uintptr_t)rootfs_src.image, rootfs_src.limit, &heap);
    }

    if (heap.start >= heap.end) {
        panic("no boot heap space available");
    }

    log_debug(
        "boot heap [%#lx, %#lx) %lu KiB",
        (unsigned long)heap.start,
        (unsigned long)heap.end,
        (unsigned long)((heap.end - heap.start) / 1024UL)
    );

    boot_heap_init(heap.start, heap.end);

    mount_rootfs(&rootfs, rootfs_src.image, rootfs_src.limit, rootfs_src.from_initrd);

    load_dtb(&rootfs, &boot_dtb, &dtb_size);

    const char *path = kernel_file();
    size_t kernel_size = 0;

    log_debug("reading kernel %s", path);

    void *kernel_blob = boot_ext2_read_file(&rootfs, path, &kernel_size);
    if (!kernel_blob) {
        panic("no kernel image found");
    }

    log_debug("loaded kernel at %#lx size=%zu KiB", (unsigned long)(uintptr_t)kernel_blob, kernel_size / 1024);

    uintptr_t elf_entry = 0;

    if (!load_elf(kernel_blob, kernel_size, &elf_entry)) {
        panic("failed to load kernel ELF");
    }

    void *dtb_copy = copy_dtb(boot_dtb, dtb_size);

    boot_desc_t desc = {
        .hartid = hartid,
        .dtb = dtb_copy,
        .dtb_size = dtb_size,
        .rootfs_image = rootfs_src.image,
        .rootfs_size = rootfs.size,
        .memory = memory,
        .uart = uart,
    };

    boot_info_t *info = make_info(&desc);

    enter_supervisor(elf_entry, info);
}
