#include <base/units.h>
#include <base/attributes.h>
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

struct riscv_elf_load_ctx {
    const u8 *blob;
};

static void boot_logf(const char *fmt, ...) {
    char buf[PRINTF_BUF_SIZE];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    printf("[riscv boot] %s\n\r", buf);
}

static bool load_kernel_segment(const elf_segment_t *seg, void *ctx) {
    const struct riscv_elf_load_ctx *load_ctx = ctx;
    const u8 *blob = load_ctx->blob;

    u64 dst = seg->paddr ? seg->paddr : seg->vaddr;
    if (!dst) {
        return false;
    }

    memcpy((void *)(uintptr_t)dst, blob + seg->offset, seg->file_size);

    if (seg->mem_size > seg->file_size) {
        void *bss = (void *)(uintptr_t)(dst + seg->file_size);
        memset(bss, 0, seg->mem_size - seg->file_size);
    }

    return true;
}

static bool load_kernel_elf(const u8 *blob, size_t blob_size, uintptr_t *out_entry) {
    struct riscv_elf_load_ctx ctx = { .blob = blob };
    elf_info_t info = {0};

    if (!elf_foreach_segment(blob, blob_size, load_kernel_segment, &ctx, &info)) {
        return false;
    }

    *out_entry = (uintptr_t)info.entry;
    return true;
}

NORETURN static void enter_supervisor(uintptr_t entry, boot_info_t *info) {
    unsigned long medeleg =
        (1UL << 0)  | (1UL << 1)  | (1UL << 2)  | (1UL << 3) |
        (1UL << 4)  | (1UL << 5)  | (1UL << 6)  | (1UL << 7) |
        (1UL << 8)  | (1UL << 12) | (1UL << 13) | (1UL << 15);
    unsigned long mideleg = MIP_SSIP | MIP_STIP | (1UL << 9);
    unsigned long mstatus = riscv_read_mstatus();

    riscv_write_medeleg(medeleg);
    riscv_write_mideleg(mideleg);
    riscv_write_mcounteren(
        RISCV_COUNTEREN_CY | RISCV_COUNTEREN_TM | RISCV_COUNTEREN_IR
    );
    riscv_write_menvcfg(MENVCFG_STCE);

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

    if (dtb && fdt_valid(dtb) && fdt_find_compatible_reg(dtb, "ns16550a", &reg) && reg.addr) {
        return (uintptr_t)reg.addr;
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
    boot_ext2_t rootfs = {0};
    bool dtb_valid = dtb && fdt_valid(dtb);
    const void *boot_dtb = dtb_valid ? dtb : NULL;
    size_t dtb_size = boot_dtb ? fdt_size(boot_dtb) : 0;

    const u8 *embedded_rootfs =
        (const u8 *)((uintptr_t)&__image_start + RISCV_BOOT_IMAGE_ROOTFS_OFFSET);

    fdt_reg_t memory_reg = detect_memory(boot_dtb);
    uintptr_t uart_base = detect_uart_base(boot_dtb);
    uintptr_t heap_start = (uintptr_t)&__stack_top;
    uintptr_t memory_end = (uintptr_t)(memory_reg.addr + memory_reg.size);
    uintptr_t scratch_base = (uintptr_t)(memory_reg.addr + RISCV_BOOT_SCRATCH_OFFSET);

    tty_set_uart_base(uart_base);

    if (scratch_base > heap_start) {
        heap_start = scratch_base;
    }

    u64 boot_hart = 0;
    bool boot_hart_known = boot_dtb && fdt_boot_cpuid_phys(boot_dtb, &boot_hart);

    if ((boot_hart_known && hartid != (uintptr_t)boot_hart) ||
        (!boot_hart_known && hartid != 0)) {
        halt();
    }

    heap_start = ALIGN(heap_start, RISCV_BOOT_PAGE_SIZE);
    memory_end = ALIGN_DOWN(memory_end, RISCV_BOOT_PAGE_SIZE);
    if (heap_start >= memory_end) {
        panic("no boot heap space available");
    }

    boot_heap_init(heap_start, memory_end);

    if (!boot_ext2_init(&rootfs, read_rootfs_image, (void *)embedded_rootfs, 0)) {
        panic("storage device not available");
    }

#if __riscv_xlen == 64
    const char *kernel_path = boot_kernel_path(true);
#else
    const char *kernel_path = boot_kernel_path(false);
#endif

    size_t kernel_size = 0;
    void *kernel_blob = boot_ext2_read_file(&rootfs, kernel_path, &kernel_size);
    if (!kernel_blob) {
        panic("no kernel image found");
    }

    boot_logf("kernel %s (%zu KiB)", kernel_path, kernel_size / 1024);

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
    }

    boot_info_t *info = boot_alloc_aligned(sizeof(*info), 16, true);
    if (!info) {
        panic("failed to allocate boot info");
    }

    init_boot_args(&info->args);
    info->hartid = hartid;
    info->dtb_paddr = (uintptr_t)dtb_copy;
    info->dtb_size = dtb_size;
    info->boot_rootfs_paddr = (uintptr_t)embedded_rootfs;
    info->boot_rootfs_size = rootfs.size;
    info->memory_paddr = memory_reg.addr;
    info->memory_size = memory_reg.size;
    info->uart_paddr = uart_base;

    enter_supervisor(elf_entry, info);
}
