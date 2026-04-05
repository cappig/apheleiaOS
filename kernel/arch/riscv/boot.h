#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <lib/boot.h>

typedef struct PACKED {
    kernel_args_t args;
    u64 hartid;
    u64 dtb_paddr;
    u64 dtb_size;
    u64 boot_rootfs_paddr;
    u64 boot_rootfs_size;
    u64 memory_paddr;
    u64 memory_size;
    u64 uart_paddr;
} boot_info_t;
