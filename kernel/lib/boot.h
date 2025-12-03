#pragma once

#include <base/attributes.h>
#include <base/types.h>

enum debug_levels : u8 {
    DEBUG_NONE = 0,
    DEBUG_MINIMAL = 1,
    DEBUG_ALL = 2,
};

enum video_mode : u8 {
    VIDEO_NONE = 0,
    VIDEO_TEXT = 1,
    VIDEO_GRAPHICS = 2,
};

// -1 means that the bootloader will attempt to autodetect
#define BOOT_DEFAULT_DEBUG       DEBUG_MINIMAL
#define BOOT_DEFAULT_VIDEO       VIDEO_GRAPHICS
#define BOOT_DEFAULT_VESA_WIDTH  -1
#define BOOT_DEFAULT_VESA_HEIGHT -1
#define BOOT_DEFAULT_VESA_BPP    32

typedef struct PACKED {
    u8 debug;

    u8 video;

    u16 vesa_width;
    u16 vesa_height;
    u16 vesa_bpp;

    char console[128];
} kernel_args_t;
