#include <arch/paging.h>
#include <base/units.h>
#include <kernel.h>
#include <parse/fdt.h>
#include <riscv/asm.h>
#include <riscv/boot.h>
#include <riscv/serial.h>
#include <string.h>

static boot_info_t direct_boot_info = {0};

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
        if (fdt_find_compatible_reg(dtb, compat_list[i], &reg) && reg.addr) {
            return (uintptr_t)reg.addr;
        }
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

static fdt_reg_t detect_initrd(const void *dtb) {
    fdt_reg_t reg = {0};

    if (dtb) {
        (void)fdt_find_initrd(dtb, &reg);
    }

    return reg;
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

static bool boot_info_valid(const boot_info_t *info) {
    if (!info || info->magic != BOOT_INFO_MAGIC) {
        return false;
    }

    if (!info->memory_paddr || !info->memory_size) {
        return false;
    }

    if (info->dtb_paddr && info->dtb_size) {
        return fdt_valid((const void *)(uintptr_t)info->dtb_paddr);
    }

    return info->uart_paddr != 0;
}

static bool boot_hart_matches(uintptr_t hartid, const void *dtb) {
    u64 boot_hart = 0;
    if (dtb && fdt_boot_cpuid_phys(dtb, &boot_hart)) {
        return hartid == (uintptr_t)boot_hart;
    }

    return hartid == 0;
}

static NORETURN void park_secondary_hart(uintptr_t uart_base, uintptr_t hartid) {
    (void)uart_base;
    (void)hartid;
    halt();
}

static boot_info_t *direct_boot_info_build(uintptr_t hartid, const void *dtb) {
    fdt_reg_t memory_reg = detect_memory(dtb);
    fdt_reg_t initrd_reg = detect_initrd(dtb);

    memset(&direct_boot_info, 0, sizeof(direct_boot_info));
    init_boot_args(&direct_boot_info.args);
    direct_boot_info.hartid = hartid;
    direct_boot_info.dtb_paddr = (uintptr_t)dtb;
    direct_boot_info.dtb_size = fdt_size(dtb);
    direct_boot_info.memory_paddr = memory_reg.addr;
    direct_boot_info.memory_size = memory_reg.size;
    direct_boot_info.boot_rootfs_paddr = initrd_reg.addr;
    direct_boot_info.boot_rootfs_size = initrd_reg.size;
    direct_boot_info.uart_paddr = detect_uart_base(dtb);

    return &direct_boot_info;
}

NORETURN void _kern_entry(uintptr_t arg0, uintptr_t arg1) {
    boot_info_t *info = NULL;
    const void *dtb = NULL;
    uintptr_t hartid = 0;
    uintptr_t uart_base = SERIAL_UART0;

    if (boot_info_valid((const boot_info_t *)arg0)) {
        info = (boot_info_t *)arg0;
        hartid = (uintptr_t)info->hartid;
        dtb = info->dtb_paddr ? (const void *)(uintptr_t)info->dtb_paddr : NULL;
        uart_base = info->uart_paddr ? (uintptr_t)info->uart_paddr : SERIAL_UART0;
    } else {
        hartid = arg0;
        dtb = arg1 ? (const void *)arg1 : NULL;
        uart_base = detect_uart_base(dtb);
        info = direct_boot_info_build(hartid, dtb);
    }

    if (!boot_hart_matches(hartid, dtb)) {
        park_secondary_hart(uart_base, hartid);
    }

    kernel_main(info);
    halt();
}
