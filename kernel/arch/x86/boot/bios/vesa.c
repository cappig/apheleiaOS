#include "vesa.h"

#include <base/macros.h>
#include <stdint.h>
#include <stdlib.h>

#include "base/types.h"
#include "bios.h"
#include "lib/boot.h"
#include "tty.h"
#include "x86/boot.h"
#include "x86/regs.h"
#include "x86/vga.h"


static bool _fetch_vbe_info(vesa_info_t* buffer) {
    regs32_t r = {0};
    r.ax = 0x4f00;
    r.edi = (u32)(uintptr_t)buffer;

    bios_call(0x10, &r, &r);

    return r.ax != 0x4f;
}

static bool _fetch_mode_info(vesa_mode_t* buffer, u16 mode) {
    regs32_t r = {0};
    r.ax = 0x4f01;
    r.cx = mode;
    r.edi = (u32)(uintptr_t)buffer;

    bios_call(0x10, &r, &r);

    return r.ax != 0x4f;
}

static bool _fetch_edid_info(edid_data_t* buffer) {
    regs32_t r = {0};
    r.ax = 0x4f15;
    r.bx = 0x01;
    r.edi = (u32)(uintptr_t)buffer;

    bios_call(0x10, &r, &r);

    return r.al != 0x4f || r.ah != 0;
}

static void _edid_resolution(u8* edid_data, edid_info_t* edid_info) {
    edid_info->monitor_width = edid_data[0x38] | ((int)(edid_data[0x3a] & 0xf0) << 4);
    edid_info->monitor_height = edid_data[0x3b] | ((int)(edid_data[0x3d] & 0xf0) << 4);
}

static void _set_vesa_mode(u16 mode_index) {
    regs32_t r = {0};
    r.ax = 0x4f02;
    r.bx = mode_index | (1 << 14); // use linear framebuffer

    bios_call(0x10, &r, &r);

    if (r.ax != 0x4f)
        panic("Failed to set VESA mode!");
}

// Find the mode with the highest possible resolution and bpp
// If a fitting mode was found it sets it up
static vesa_mode_t _init_vesa(u16 max_width, u16 max_height, u16 max_bpp) {
    vesa_info_t info_buffer = {0};
    vesa_mode_t current_mode = {0}, best_mode = {0};

    if (_fetch_vbe_info(&info_buffer))
        return current_mode;

    u16* mode_ptr = (u16*)REAL_FLATTEN(info_buffer.video_mode_seg, info_buffer.video_mode_off);

    // u16* mode_ptr = (u16*)(uintptr_t)info_buffer.video_mode;

    size_t best_mode_i = 0;

    for (size_t i = 0; mode_ptr[i] != 0xffff; i++) {
        if (_fetch_mode_info(&current_mode, mode_ptr[i]))
            continue;

        // printf("2~~~> %d\n\r", current_mode.memory_model);

        // Check if mode is direct color
        if (current_mode.memory_model != 0x06)
            continue;

        // Check if mode supports linear frame buffer
        if ((current_mode.attributes & 0x90) != 0x90)
            continue;

        // Ignore modes that are too large
        if (current_mode.width > max_width || current_mode.height > max_height ||
            current_mode.bits_per_pixel > max_bpp * 8) {
            continue;
        }

        if (current_mode.width > best_mode.width || current_mode.height > best_mode.height ||
            current_mode.bits_per_pixel > best_mode.bits_per_pixel) {
            best_mode_i = mode_ptr[i];
            best_mode = current_mode;
        }
    }

    if (best_mode.bits_per_pixel)
        _set_vesa_mode(best_mode_i);

    return best_mode;
}


void init_graphics(boot_info_t* info) {
    if (info->args.video == VIDEO_NONE)
        return;

    video_info_t* video = &info->video;
    edid_info_t* edid = &info->edid;
    kernel_args_t* args = &info->args;

    edid_data_t edid_data = {0};

    if (!_fetch_edid_info(&edid_data)) {
        _edid_resolution((u8*)&edid_data, &info->edid);

        video->width = min(args->vesa_width, edid->monitor_width);
        video->height = min(args->vesa_height, edid->monitor_height);

        printf("detected monitor: %dx%d\n\r", video->width, video->height);
    }

    if (info->args.video == VIDEO_GRAPHICS) {
        vesa_mode_t vesa = _init_vesa(info->video.width, info->video.height, info->args.vesa_bpp);

        if (vesa.bits_per_pixel) {
            video->mode = VIDEO_GRAPHICS;

            video->framebuffer = (u64)vesa.framebuffer;

            video->width = vesa.width;
            video->height = vesa.height;

            video->bytes_per_pixel = (u32)vesa.bits_per_pixel / 8;
            video->bytes_per_line = (u32)vesa.bytes_per_line;

            video->red_mask = vesa.red.mask;
            video->green_mask = vesa.green.mask;
            video->blue_mask = vesa.blue.mask;

            printf("video output: graphics %hdx%hd\n\r", video->width, video->height);

            return;
        }
    }

    printf("video output: vga text\n\r");

    video->mode = VIDEO_TEXT;

    video->framebuffer = VGA_ADDR;

    video->width = VGA_WIDTH;
    video->height = VGA_HEIGHT;
}
