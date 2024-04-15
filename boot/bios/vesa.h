#pragma once

#include <base/addr.h>
#include <base/attributes.h>
#include <base/types.h>
#include <boot/proto.h>

// Implementations of VESA data structures
// https://pdos.csail.mit.edu/6.828/2018/readings/hardware/vbe3.pdf

typedef struct PACKED {
    u8 signature[4];
    u16 version;

    u32_addr oem_string;
    u32 capabilities;
    u32_addr video_mode;
    u16 total_memory_blocks;

    u16 oem_software_rev;
    u32_addr oem_vendor_name;
    u32_addr oem_product_name;
    u32_addr oem_product_rev;

    u8 _unused0[222];
    u8 _oem_data[256];
} vesa_info;

typedef struct PACKED {
    u8 size;
    u8 position;
} vesa_color;

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

    vesa_color red;
    vesa_color green;
    vesa_color blue;
    vesa_color reserved;
    u8 color_attributes;

    u32 framebuffer;
    u32 offscreen_mem;
    u16 offscreen_size;

    u8 _unused1[206];
} vesa_mode;


void init_graphics(graphics_state* gfx, u8 mode, u16 width, u16 height, u16 bpp);
