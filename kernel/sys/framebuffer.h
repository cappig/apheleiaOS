#pragma once

#include <base/types.h>
#include <stdbool.h>

typedef struct framebuffer_info {
    u64 paddr;
    u64 size;
    u32 width;
    u32 height;
    u32 pitch;
    u8 bpp;
    u8 red_mask;
    u8 green_mask;
    u8 blue_mask;
    bool available;
} framebuffer_info_t;

void framebuffer_set_info(const framebuffer_info_t* info);
const framebuffer_info_t* framebuffer_get_info(void);
