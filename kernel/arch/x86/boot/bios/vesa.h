#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <lib/boot.h>

#include "x86/lib/boot.h"

// Implementations of VESA data structures
// https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf

typedef struct PACKED {
    u8 signature[4];
    u16 version;

    u32 oem_string;
    u32 capabilities;
    // u32 video_mode;
    u16 video_mode_off;
    u16 video_mode_seg;
    u16 total_memory_blocks;

    u16 oem_software_rev;
    u32 oem_vendor_name;
    u32 oem_product_name;
    u32 oem_product_rev;

    u8 _unused0[222];
    u8 _oem_data[256];
} vesa_info_t;

typedef struct PACKED {
    u8 position;
    u8 mask;
} vesa_color_t;

typedef struct PACKED {
    u16 attributes;

    u8 window[2];
    u16 w_granularity;
    u16 w_size;
    u16 w_segment[2];
    u32 w_func_ptr;

    u16 bytes_per_line;
    u16 width;
    u16 height;

    u8 char_width;
    u8 char_height;
    u8 planes;

    u8 bits_per_pixel;

    u8 banks;
    u8 memory_model;
    u8 bank_size;
    u8 image_pages;
    u8 _unused0;

    vesa_color_t red;
    vesa_color_t green;
    vesa_color_t blue;
    vesa_color_t reserved;
    u8 color_attributes;

    u32 framebuffer;
    u32 offscreen_mem;
    u16 offscreen_size;

    u8 _unused1[206];
} vesa_mode_t;

// https://wiki.osdev.org/EDID
// https://en.wikipedia.org/wiki/Extended_Display_Identification_Data

typedef struct PACKED {
    u8 _unused1[8];

    u16 manufacture_id_msb;
    u16 edid_id;
    u32 serial_num;

    u8 manufacture_week;
    u8 manufacture_year;

    u8 edid_version;
    u8 edid_revision;

    u8 video_input_type;

    u8 max_hor_size_cm;
    u8 max_ver_size_cm;

    u8 gamma_factor;
    u8 dpms_flags;
    u8 chroma_info[10];

    u8 timings_1;
    u8 timings_2;
    u8 manufacture_timing;

    u16 timing_id[8];
    u8 timing_desc_1[18];
    u8 timing_desc_2[18];
    u8 timing_desc_3[18];
    u8 timing_desc_4[18];

    u8 _unused2;

    u8 checksum;
} edid_data_t;


void init_graphics(boot_info_t* info);
