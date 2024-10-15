#pragma once

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <gfx/state.h>
#include <x86/e820.h>

#define BOOT_MAGIC 0xA76e1e1a

#define KERNEL_STACK_SIZE (256 * KiB)
#define KERNEL_HEAP_PAGES 512

// -1 means that the bootloader will attempt to autodetect
#define BOOT_DEFAULT_GFX_MODE    GFX_VESA
#define BOOT_DEFAULT_VESA_WIDTH  -1
#define BOOT_DEFAULT_VESA_HEIGHT -1
#define BOOT_DEFAULT_VESA_BPP    32
#define BOOT_DEFAULT_SERIAL_BAUD 9600

#define BOOT_CONSOLE_FONT_LEN 20

#define BOOT_FALLBACK_VESA_WIDTH  1280
#define BOOT_FALLBACK_VESA_HEIGHT 720

// Options that can be set via args.cfg
typedef struct PACKED {
    u8 gfx_mode;

    u16 vesa_width;
    u16 vesa_height;
    u16 vesa_bpp;

    u32 serial_baud;

    char console_font[BOOT_CONSOLE_FONT_LEN + 1];
} boot_args;

typedef struct PACKED {
    u32 magic;
    u32 chacksum; // TODO:

    u64 stack_top;
    u64 rsdp;

    u32 initrd_loc;
    u32 initrd_size;

    u32 symtab_loc;
    u32 symtab_size;

    boot_args args;
    e820_map mmap;
    graphics_state graphics;
} boot_handoff;
