#include <base/units.h>
#include <base/attributes.h>
#include <riscv/asm.h>
#include <riscv/boot.h>
#include <riscv/serial.h>
#include <common/elf.h>
#include <lib/boot.h>
#include <parse/fdt.h>
#include <stdlib.h>
#include <string.h>

#include <common/ext2.h>

#include "memory.h"
#include "mtrap.h"
#include "tty.h"
#include "virtio.h"

extern char __stack_top;
extern char __image_start;

#define RISCV_BOOT_PAGE_SIZE     4096UL
#define RISCV_BOOT_SCRATCH_OFFSET (32ULL * MIB)
#ifndef RISCV_BOOT_IMAGE_ROOTFS_OFFSET
#define RISCV_BOOT_IMAGE_ROOTFS_OFFSET (8ULL * MIB)
#endif
#define RISCV_MCAUSE_ECALL_S     9
#define RISCV_MCAUSE_M_TIMER     7
#define RISCV_CLINT_MTIMECMP_OFF 0x4000UL
#define RISCV_CLINT_DEFAULT_BASE 0x02000000UL
#define RISCV_SIFIVE_TEST_SHUTDOWN 0x5555U
#define RISCV_SIFIVE_TEST_REBOOT   0x7777U

#define SBI_EXT_BASE  0x10
#define SBI_EXT_TIME  0x54494d45
#define SBI_EXT_SRST  0x53525354
#define SBI_EXT_APHE  0x41504845

#define SBI_SUCCESS            0L
#define SBI_ERR_NOT_SUPPORTED -2L

#define SBI_BASE_PROBE_EXTENSION 3
#define SBI_SRST_RESET_TYPE_SHUTDOWN 0
#define SBI_SRST_RESET_TYPE_COLD_REBOOT 1
#define SBI_APHE_FID_SOFTIRQ_SET   0
#define SBI_APHE_FID_SOFTIRQ_CLEAR 1

extern void riscv_boot_mtrap_entry(void);

static uintptr_t sbi_uart_base = SERIAL_UART0;
static uintptr_t sbi_clint_base = 0;
static uintptr_t sbi_test_base = 0;
static uintptr_t sbi_hartid = 0;

static void clint_set_timer(u64 next);

struct riscv_elf_load_ctx {
    const u8 *blob;
};

static bool load_kernel_segment(
    const elf_segment_t *seg,
    void *ctx
) {
    const struct riscv_elf_load_ctx *load_ctx = ctx;
    const u8 *blob = load_ctx->blob;

    u64 dst = seg->paddr ? seg->paddr : seg->vaddr;
    if (!dst) {
        return false;
    }

    memcpy((void *)(uintptr_t)dst, blob + seg->offset, seg->file_size);
    if (seg->mem_size > seg->file_size) {
        memset(
            (void *)(uintptr_t)(dst + seg->file_size),
            0,
            seg->mem_size - seg->file_size
        );
    }

    return true;
}

static bool load_kernel_elf(
    const u8 *blob,
    size_t blob_size,
    uintptr_t *out_entry
) {
    struct riscv_elf_load_ctx ctx = {
        .blob = blob,
    };
    elf_info_t info = {0};

    if (!elf_foreach_segment(blob, blob_size, load_kernel_segment, &ctx, &info)) {
        return false;
    }

    *out_entry = (uintptr_t)info.entry;
    return true;
}

NORETURN static void enter_supervisor(uintptr_t entry, boot_info_t *info) {
    unsigned long medeleg =
        (1UL << 0) |
        (1UL << 1) |
        (1UL << 2) |
        (1UL << 3) |
        (1UL << 4) |
        (1UL << 5) |
        (1UL << 6) |
        (1UL << 7) |
        (1UL << 8) |
        (1UL << 12) |
        (1UL << 13) |
        (1UL << 15);
    unsigned long mideleg = MIP_SSIP | MIP_STIP | (1UL << 9);
    unsigned long mstatus = riscv_read_mstatus();

    riscv_write_mtvec((uintptr_t)riscv_boot_mtrap_entry);
    riscv_write_mscratch((uintptr_t)&__stack_top);
    riscv_write_medeleg(medeleg);
    riscv_write_mideleg(mideleg);
    riscv_write_mcounteren(
        RISCV_COUNTEREN_CY | RISCV_COUNTEREN_TM | RISCV_COUNTEREN_IR
    );
    clint_set_timer(~0ULL);
    riscv_write_mie(0);
    riscv_clear_mip_bits(MIP_SSIP | MIP_STIP);

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
        : "r"(info), "r"(sbi_hartid)
        : "memory"
    );

    __builtin_unreachable();
}

static uintptr_t detect_uart_base(const void *dtb) {
    if (!dtb || !fdt_valid(dtb)) {
        return SERIAL_UART0;
    }

    static const char *const compat_list[] = {
        "ns16550a",
        "sifive,uart0",
        "uart8250",
    };

    fdt_reg_t reg = {0};
    for (size_t i = 0; i < sizeof(compat_list) / sizeof(compat_list[0]); i++) {
        if (fdt_find_compatible_reg(dtb, compat_list[i], &reg)) {
            if (reg.addr) {
                return (uintptr_t)reg.addr;
            }
        }
    }

    return SERIAL_UART0;
}

static fdt_reg_t detect_memory(const void *dtb) {
    fdt_reg_t reg = {
        .addr = 0x80000000ULL,
        .size = 256ULL * MIB,
    };

    fdt_reg_t probed = {0};
    if (dtb && fdt_find_memory_reg(dtb, &probed) && probed.addr && probed.size) {
        reg = probed;
    }

    return reg;
}

static uintptr_t detect_clint_base(const void *dtb) {
    fdt_reg_t reg = {0};

    if (dtb && fdt_find_compatible_reg(dtb, "riscv,clint0", &reg) && reg.addr) {
        return (uintptr_t)reg.addr;
    }

    if (dtb && fdt_find_compatible_reg(dtb, "sifive,clint0", &reg) && reg.addr) {
        return (uintptr_t)reg.addr;
    }

    if (dtb && fdt_find_compatible_reg(dtb, "riscv,aclint-mtimer", &reg) && reg.addr) {
        return (uintptr_t)reg.addr;
    }

    return RISCV_CLINT_DEFAULT_BASE;
}

static uintptr_t detect_test_base(const void *dtb) {
    fdt_reg_t reg = {0};

    if (dtb && fdt_find_compatible_reg(dtb, "sifive,test0", &reg) && reg.addr) {
        return (uintptr_t)reg.addr;
    }

    return 0;
}

static void init_boot_args(kernel_args_t *args) {
    if (!args) {
        return;
    }

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

static void clint_set_timer(u64 next) {
    if (!sbi_clint_base) {
        return;
    }

    uintptr_t cmp_addr =
        sbi_clint_base + RISCV_CLINT_MTIMECMP_OFF + (sbi_hartid * sizeof(u64));

#if __riscv_xlen == 64
    *(volatile u64 *)cmp_addr = next;
#else
    volatile u32 *cmp = (volatile u32 *)cmp_addr;
    cmp[1] = 0xffffffffU;
    cmp[0] = (u32)next;
    cmp[1] = (u32)(next >> 32);
#endif

    mmio_fence();
}

static void sbi_return(
    riscv_mtrap_frame_t *frame,
    long error,
    long value
) {
    frame->g_regs.a0 = (uintptr_t)error;
    frame->g_regs.a1 = (uintptr_t)value;
    frame->m_regs.mepc += 4;
}

static long sbi_probe_extension(long ext) {
    switch (ext) {
    case SBI_EXT_BASE:
    case SBI_EXT_TIME:
    case SBI_EXT_SRST:
    case SBI_EXT_APHE:
        return 1;
    default:
        return 0;
    }
}

static NORETURN void sbi_system_reset(uintptr_t type) {
    if (sbi_test_base) {
        volatile u32 *test = (volatile u32 *)sbi_test_base;
        *test =
            (type == SBI_SRST_RESET_TYPE_COLD_REBOOT)
                ? RISCV_SIFIVE_TEST_REBOOT
                : RISCV_SIFIVE_TEST_SHUTDOWN;
    }

    halt();
}

void riscv_boot_handle_trap(riscv_mtrap_frame_t *frame) {
    if (!frame) {
        panic("missing machine trap frame");
    }

#if __riscv_xlen == 64
    bool interrupt = (frame->m_regs.mcause >> 63) != 0;
    u64 cause = frame->m_regs.mcause & ~(1ULL << 63);
#else
    bool interrupt = (frame->m_regs.mcause >> 31) != 0;
    u64 cause = frame->m_regs.mcause & ~(1ULL << 31);
#endif

    if (interrupt) {
        if (cause == RISCV_MCAUSE_M_TIMER) {
            clint_set_timer(~0ULL);
            riscv_clear_mie_bits(MIE_MTIE);
            riscv_set_mip_bits(MIP_STIP);
            return;
        }
    } else if (cause == RISCV_MCAUSE_ECALL_S) {
        uintptr_t ext = frame->g_regs.a7;
        uintptr_t fid = frame->g_regs.a6;

        switch (ext) {
        case SBI_EXT_BASE:
            if (fid == SBI_BASE_PROBE_EXTENSION) {
                sbi_return(
                    frame,
                    SBI_SUCCESS,
                    sbi_probe_extension((long)frame->g_regs.a0)
                );
                return;
            }
            break;
        case SBI_EXT_TIME: {
#if __riscv_xlen == 64
            u64 next = frame->g_regs.a0;
#else
            u64 next = ((u64)frame->g_regs.a1 << 32) | (u32)frame->g_regs.a0;
#endif
            riscv_clear_mip_bits(MIP_STIP);
            clint_set_timer(next);
            riscv_set_mie_bits(MIE_MTIE);
            sbi_return(frame, SBI_SUCCESS, 0);
            return;
        }
        case SBI_EXT_SRST:
            sbi_return(frame, SBI_SUCCESS, 0);
            sbi_system_reset(frame->g_regs.a0);
        case SBI_EXT_APHE:
            switch (fid) {
            case SBI_APHE_FID_SOFTIRQ_SET:
                riscv_set_sip_bits(SIP_SSIP);
                sbi_return(frame, SBI_SUCCESS, 0);
                return;
            case SBI_APHE_FID_SOFTIRQ_CLEAR:
                riscv_clear_sip_bits(SIP_SSIP);
                sbi_return(frame, SBI_SUCCESS, 0);
                return;
            default:
                break;
            }
        default:
            break;
        }

        sbi_return(frame, SBI_ERR_NOT_SUPPORTED, 0);
        return;
    }

    serial_printf(
        "unhandled m-trap cause=0x%lx mepc=0x%lx mtval=0x%lx\n\r",
        (unsigned long)frame->m_regs.mcause,
        (unsigned long)frame->m_regs.mepc,
        (unsigned long)frame->m_regs.mtval
    );
    halt();
}

static bool read_rootfs_disk(
    void *dest,
    size_t offset,
    size_t bytes,
    void *ctx
) {
    (void)ctx;
    u8 *out = (u8 *)dest;
    size_t sector = offset / 512;
    size_t sector_offset = offset % 512;

    while (bytes) {
        u8 scratch[512];
        size_t chunk = 512 - sector_offset;

        if (chunk > bytes) {
            chunk = bytes;
        }

        if (sector_offset == 0 && chunk == 512) {
            if (!virtio_read(sector, out, 1)) {
                return false;
            }
        } else {
            if (!virtio_read(sector, scratch, 1)) {
                return false;
            }
            memcpy(out, scratch + sector_offset, chunk);
        }

        out += chunk;
        bytes -= chunk;
        sector++;
        sector_offset = 0;
    }

    return true;
}

static bool read_rootfs_image(
    void *dest,
    size_t offset,
    size_t bytes,
    void *ctx
) {
    const u8 *image = (const u8 *)ctx;

    if (!dest || !image) {
        return false;
    }

    memcpy(dest, image + offset, bytes);
    return true;
}

NORETURN void boot_main(uintptr_t hartid, const void *dtb) {
    boot_ext2_t rootfs = {0};
    fdt_reg_t virtio_regs[16];
    size_t virtio_count = 0;
    uintptr_t embedded_rootfs_addr =
        (uintptr_t)&__image_start + RISCV_BOOT_IMAGE_ROOTFS_OFFSET;
    const u8 *embedded_rootfs = (const u8 *)embedded_rootfs_addr;
    fdt_reg_t memory_reg = detect_memory(dtb);
    uintptr_t uart_base = detect_uart_base(dtb);
    uintptr_t heap_start = (uintptr_t)&__stack_top;
    uintptr_t memory_end = (uintptr_t)(memory_reg.addr + memory_reg.size);

    uintptr_t scratch_base = (uintptr_t)(memory_reg.addr + RISCV_BOOT_SCRATCH_OFFSET);
    if (scratch_base > heap_start) {
        heap_start = scratch_base;
    }

    u64 boot_hart = 0;
    bool boot_hart_known = dtb && fdt_boot_cpuid_phys(dtb, &boot_hart);
    if ((boot_hart_known && hartid != (uintptr_t)boot_hart) ||
        (!boot_hart_known && hartid != 0)) {
        tty_set_uart_base(uart_base);
        printf("parking secondary hart %lu\n\r", (unsigned long)hartid);
        halt();
    }

    heap_start = ALIGN(heap_start, RISCV_BOOT_PAGE_SIZE);
    memory_end = ALIGN_DOWN(memory_end, RISCV_BOOT_PAGE_SIZE);
    if (heap_start >= memory_end) {
        panic("no boot heap space available");
    }

    boot_heap_init(heap_start, memory_end);

    tty_set_uart_base(uart_base);
    sbi_uart_base = uart_base;
    sbi_clint_base = detect_clint_base(dtb);
    sbi_test_base = detect_test_base(dtb);
    sbi_hartid = hartid;

    if (dtb && fdt_valid(dtb)) {
        fdt_find_compatible_regs(
            dtb,
            "virtio,mmio",
            virtio_regs,
            sizeof(virtio_regs) / sizeof(virtio_regs[0]),
            &virtio_count
        );
    }

    printf("starting apheleiaOS\n\r");

    if (!boot_ext2_init(&rootfs, read_rootfs_image, (void *)embedded_rootfs, 0)) {
        if (!virtio_init(virtio_regs, virtio_count)) {
            panic("storage device not available");
        }
        if (!boot_ext2_init(&rootfs, read_rootfs_disk, NULL, 0)) {
            panic("not an ext2 filesystem");
        }
    }

    const char *kernel_path = boot_kernel_path(true);
#if __riscv_xlen == 32
    kernel_path = boot_kernel_path(false);
#endif

    size_t kernel_size = 0;
    void *kernel_blob =
        boot_ext2_read_file(&rootfs, kernel_path, &kernel_size);

    if (!kernel_blob) {
        panic("no kernel image found");
    }

    uintptr_t elf_entry = 0;
    if (!load_kernel_elf(kernel_blob, kernel_size, &elf_entry)) {
        puts("failed to load kernel ELF\n\r");
        halt();
    }

    void *rootfs_copy =
        boot_alloc_aligned(rootfs.size, RISCV_BOOT_PAGE_SIZE, false);
    if (!rootfs_copy) {
        panic("failed to stage rootfs");
    }
    if (!rootfs.read(rootfs_copy, 0, rootfs.size, rootfs.ctx)) {
        panic("failed to read rootfs image");
    }

    size_t dtb_size = fdt_size(dtb);
    void *dtb_copy = NULL;
    if (dtb && dtb_size) {
        dtb_copy = boot_alloc_aligned(dtb_size, RISCV_BOOT_PAGE_SIZE, false);
        if (!dtb_copy) {
            panic("failed to copy dtb");
        }
        memcpy(dtb_copy, dtb, dtb_size);
    }

    boot_info_t *info = boot_alloc_aligned(sizeof(*info), 16, true);
    if (!info) {
        panic("failed to allocate boot info");
    }

    init_boot_args(&info->args);
    info->hartid = hartid;
    info->dtb_paddr = (uintptr_t)dtb_copy;
    info->dtb_size = dtb_size;
    info->boot_rootfs_paddr = (uintptr_t)rootfs_copy;
    info->boot_rootfs_size = rootfs.size;
    info->memory_paddr = memory_reg.addr;
    info->memory_size = memory_reg.size;
    info->uart_paddr = uart_base;

    serial_printf("jumping to kernel at 0x%lx\n\r", (unsigned long)elf_entry);
    enter_supervisor(elf_entry, info);
}
