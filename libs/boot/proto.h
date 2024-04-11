#pragma once

#include <base/attributes.h>
#include <base/types.h>
#include <gfx/state.h>
#include <x86/e820.h>

#define BOOT_MAGIC 0xA76e1e1a

#define BOOT_STACK_SIZE 0x8000

typedef struct PACKED {
    u8 mode;

    u8 depth;
    u16 pitch;

    u16 width;
    u16 height;

    u64 framebuffer;
} boot_graphics;

typedef struct PACKED {
    u32 magic;
    u32 chacksum; // TODO:

    u64 stack_top;

    e820_map mmap;

    boot_graphics graphics;
} boot_handoff;
