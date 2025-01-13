#pragma once

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <gfx/state.h>
#include <x86/e820.h>

#define BOOT_MAGIC 0xA76e1e1a

#define KERNEL_STACK_PAGES 16

#define INITRD_FONT_NAME   "font.psf"
#define INITRD_SYMTAB_NAME "sym.map"

// -1 means that the bootloader will attempt to autodetect
#define BOOT_DEFAULT_GFX_MODE    GFX_VESA
#define BOOT_DEFAULT_VESA_WIDTH  -1
#define BOOT_DEFAULT_VESA_HEIGHT -1
#define BOOT_DEFAULT_VESA_BPP    32

#define BOOT_FALLBACK_VESA_WIDTH  1280
#define BOOT_FALLBACK_VESA_HEIGHT 720

// Options that can be set via args.cfg
typedef struct PACKED {
    u8 gfx_mode;

    u16 vesa_width;
    u16 vesa_height;
    u16 vesa_bpp;
} boot_args;

typedef struct PACKED {
    u32 checksum; // TODO: implement this
    u32 magic;

    u64 stack_top;
    u64 rsdp;

    u32 initrd_loc;
    u32 initrd_size;

    boot_args args;

    e820_map mmap;

    graphics_state graphics;
} boot_handoff;
