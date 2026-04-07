#pragma once

#include <base/attributes.h>
#include <base/types.h>

typedef enum {
    DEBUG_NONE = 0,
    DEBUG_MINIMAL = 1,
    DEBUG_ALL = 2,
} debug_levels_t;

typedef enum {
    VIDEO_NONE = 0,
    VIDEO_TEXT = 1,
    VIDEO_GRAPHICS = 2,
} video_mode_t;

// -1 means that the bootloader will attempt to autodetect
#define BOOT_DEFAULT_DEBUG       DEBUG_MINIMAL
#define BOOT_DEFAULT_VIDEO       VIDEO_TEXT
#define BOOT_DEFAULT_VESA_WIDTH  -1
#define BOOT_DEFAULT_VESA_HEIGHT -1
#define BOOT_DEFAULT_VESA_BPP    32
#define BOOT_DEFAULT_FONT        "/etc/ter-116n.psf"
#define BOOT_DEFAULT_STAGE_ROOTFS 0

#define BOOT_KERNEL_PATH_32 "/boot/kernel32.elf"
#define BOOT_KERNEL_PATH_64 "/boot/kernel64.elf"

static inline const char *boot_kernel_path(bool is_64) {
    return is_64 ? BOOT_KERNEL_PATH_64 : BOOT_KERNEL_PATH_32;
}

typedef struct PACKED {
    u8 debug;
    u8 stage_rootfs;

    u8 video;

    u16 vesa_width;
    u16 vesa_height;
    u16 vesa_bpp;

    char console[128];
    char font[128];
} kernel_args_t;
