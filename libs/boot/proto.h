#pragma once

#include <base/attributes.h>
#include <base/macros.h>
#include <base/types.h>
#include <gfx/state.h>
#include <x86/e820.h>

#define BOOT_MAGIC 0xA76e1e1a

#define KERNEL_STACK_SIZE (16 * KiB)
#define KERNEL_HEAP_PAGES 512

typedef struct PACKED {
    u32 magic;
    u32 chacksum; // TODO:

    u64 stack_top;

    u64 rsdp;

    e820_map mmap;

    graphics_state graphics;
} boot_handoff;
