#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <base/units.h>
#include <lib/boot.h>

#include "e820.h"

#define LINEAR_MAP_OFFSET_64 0xffff888000000000ULL
#define LINEAR_MAP_OFFSET_32 0xc0000000UL

#define PHYS_WINDOW_BASE_32 0xff000000UL
#define PHYS_WINDOW_SIZE_32 (16 * MIB)

#define PROTECTED_MODE_TOP 0x100000000ULL

#define KERNEL_STACK_SIZE (16 * 0x1000)

typedef struct PACKED {
    u8 mode;

    u8 red_shift;
    u8 green_shift;
    u8 blue_shift;

    u8 red_size;
    u8 green_size;
    u8 blue_size;

    u16 bytes_per_pixel; // 'depth'
    u16 bytes_per_line; // 'pitch'

    u16 width;
    u16 height;

    u64 framebuffer;
} video_info_t;

typedef struct PACKED {
    u16 manufacturer_id;
    u16 product_code;

    u16 monitor_width;
    u16 monitor_height;
} edid_info_t;

typedef enum PACKED {
    BOOT_MEDIA_UNKNOWN = 0,
    BOOT_MEDIA_DISK = 1,
    BOOT_MEDIA_USB = 2,
    BOOT_MEDIA_OPTICAL = 3,
    BOOT_MEDIA_NETWORK = 4,
} boot_media_t;

typedef enum PACKED {
    BOOT_TRANSPORT_UNKNOWN = 0,
    BOOT_TRANSPORT_ATA = 1,
    BOOT_TRANSPORT_AHCI = 2,
    BOOT_TRANSPORT_ATAPI = 3,
    BOOT_TRANSPORT_USB = 4,
    BOOT_TRANSPORT_NVME = 5,
} boot_transport_t;

typedef enum PACKED {
    BOOT_PARTSTYLE_UNKNOWN = 0,
    BOOT_PARTSTYLE_MBR = 1,
    BOOT_PARTSTYLE_GPT = 2,
} boot_part_style_t;

typedef struct PACKED {
    u8 valid;
    u8 media;
    u8 transport;
    u8 part_style;
    u8 part_index;
    u8 bios_drive;
    u8 rootfs_uuid_valid;
    u8 _reserved0;
    u8 rootfs_uuid[16];
} boot_root_hint_t;


typedef struct PACKED {
    kernel_args_t args;

    u64 acpi_root_ptr;
    u64 boot_rootfs_paddr;
    u64 boot_rootfs_size;

    video_info_t video;
    edid_info_t edid;

    e820_map_t memory_map;

    boot_root_hint_t boot_root_hint;
    u64 smp_trampoline_paddr;
} boot_info_t;
