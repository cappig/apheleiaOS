#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <lib/boot.h>

#include "e820.h"

typedef struct PACKED {
    u8 mode;

    u8 red_mask;
    u8 green_mask;
    u8 blue_mask;

    u16 bytes_per_pixel; // 'depth'
    u16 bytes_per_line; // 'pitch'

    u16 width;
    u16 height;

    u32 framebuffer;
} video_info_t;

typedef struct PACKED {
    u16 manufacturer_id;
    u16 product_code;

    u16 monitor_width;
    u16 monitor_height;
} edid_info_t;


typedef struct PACKED {
    kernel_args_t args;

    u64 acpi_root_ptr;

    video_info_t video;
    edid_info_t edid;

    e820_map_t memory_map;
} boot_info_t;
