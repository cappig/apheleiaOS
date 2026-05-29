#include "vesa.h"

#include <base/macros.h>
#include <log/log.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "base/types.h"
#include "bios.h"
#include "lib/boot.h"
#include "tty.h"
#include "x86/boot.h"
#include "x86/regs.h"
#include "x86/vga.h"

#define VESA_LINEAR_FRAMEBUFFER (1 << 14)

static bool fetch_vbe(vesa_info_t *buffer) {
    if (!buffer) {
        return true;
    }

    memcpy(buffer->signature, "VBE2", 4);

    regs32_t r = { 0 };
    r.ax = 0x4f00;
    r.es = REAL_SEG(buffer);
    r.edi = REAL_OFF(buffer);

    bios_call(0x10, &r, &r);

    if (r.al != 0x4f || r.ah != 0x00) {
        return true;
    }

    return memcmp(buffer->signature, "VESA", 4) != 0;
}

static bool fetch_mode(vesa_mode_t *buffer, u16 mode) {
    if (!buffer) {
        return true;
    }

    regs32_t r = { 0 };

    r.ax = 0x4f01;
    r.cx = mode;

    r.es = REAL_SEG(buffer);
    r.edi = REAL_OFF(buffer);

    bios_call(0x10, &r, &r);

    return r.al != 0x4f || r.ah != 0x00;
}

static bool fetch_edid(edid_data_t *buffer) {
    if (!buffer) {
        return true;
    }

    regs32_t r = { 0 };

    r.ax = 0x4f15;
    r.bx = 0x01;

    r.es = REAL_SEG(buffer);
    r.edi = REAL_OFF(buffer);

    bios_call(0x10, &r, &r);

    return r.al != 0x4f || r.ah != 0;
}

static void edid_res(u8 *edid_data, edid_info_t *edid_info) {
    edid_info->monitor_width = edid_data[0x38] | ((int)(edid_data[0x3a] & 0xf0) << 4);

    edid_info->monitor_height = edid_data[0x3b] | ((int)(edid_data[0x3d] & 0xf0) << 4);
}

static bool set_mode(u16 mode_index) {
    regs32_t r = { 0 };
    r.ax = 0x4f02;
    r.bx = mode_index | VESA_LINEAR_FRAMEBUFFER;

    bios_call(0x10, &r, &r);

    return r.al == 0x4f && r.ah == 0;
}

static vesa_mode_t pick_mode(u16 max_width, u16 max_height, u16 max_bpp) {
    vesa_info_t info_buffer = { 0 };
    vesa_mode_t current_mode = { 0 }, best_mode = { 0 };

    if (fetch_vbe(&info_buffer)) {
        return current_mode;
    }

    uintptr_t mode_list = REAL_FLATTEN(info_buffer.video_mode_seg, info_buffer.video_mode_off);

    if (mode_list < 0x0500 || mode_list > 0x10fff0) {
        return current_mode;
    }

    if (!max_width) {
        max_width = 0xffff;
    }

    if (!max_height) {
        max_height = 0xffff;
    }

    if (!max_bpp) {
        max_bpp = BOOT_DEFAULT_VESA_BPP;
    }

    u16 *mode_ptr = (u16 *)mode_list;

    u16 best_mode_i = 0;

    for (size_t i = 0; i < 1024; i++) {
        u16 mode = mode_ptr[i];

        if (mode == 0xffff) {
            break;
        }

        if (fetch_mode(&current_mode, mode)) {
            continue;
        }

        if (current_mode.memory_model != 0x06) {
            continue;
        }

        if ((current_mode.attributes & 0x90) != 0x90) {
            continue;
        }

        if (current_mode.width > max_width || current_mode.height > max_height ||
            current_mode.bits_per_pixel > max_bpp) {
            continue;
        }

        if (current_mode.width > best_mode.width || current_mode.height > best_mode.height ||
            current_mode.bits_per_pixel > best_mode.bits_per_pixel) {
            best_mode_i = mode;
            best_mode = current_mode;
        }
    }

    if (best_mode.bits_per_pixel && set_mode(best_mode_i)) {
        return best_mode;
    }

    return (vesa_mode_t){ 0 };
}

static void text_video(video_info_t *video) {
    video->mode = VIDEO_TEXT;
    video->framebuffer = VGA_ADDR;

    video->width = VGA_WIDTH;
    video->height = VGA_HEIGHT;

    video->bytes_per_pixel = 2;
    video->bytes_per_line = VGA_WIDTH * 2;

    video->red_shift = 0;
    video->green_shift = 0;
    video->blue_shift = 0;

    video->red_size = 0;
    video->green_size = 0;
    video->blue_size = 0;
}

void init_graphics(boot_info_t *info) {
    if (!info) {
        return;
    }

    video_info_t *video = &info->video;
    edid_info_t *edid = &info->edid;
    kernel_args_t *args = &info->args;

    text_video(video);

    if (info->args.video == VIDEO_NONE) {
        return;
    }

    edid_data_t edid_data = { 0 };

    if (!fetch_edid(&edid_data)) {
        edid_res((u8 *)&edid_data, &info->edid);

        video->width = min(args->vesa_width, edid->monitor_width);
        video->height = min(args->vesa_height, edid->monitor_height);

        log_debug("detected monitor %ux%u", (unsigned int)video->width, (unsigned int)video->height);
    }

    if (info->args.video == VIDEO_GRAPHICS) {
        vesa_mode_t vesa = pick_mode(video->width, video->height, info->args.vesa_bpp);

        if (vesa.bits_per_pixel) {
            video->mode = VIDEO_GRAPHICS;
            video->framebuffer = (u64)vesa.framebuffer;

            video->width = vesa.width;
            video->height = vesa.height;

            video->bytes_per_pixel = (u32)vesa.bits_per_pixel / 8;
            video->bytes_per_line = (u32)vesa.bytes_per_line;

            video->red_shift = vesa.red.position;
            video->green_shift = vesa.green.position;
            video->blue_shift = vesa.blue.position;

            video->red_size = vesa.red.mask;
            video->green_size = vesa.green.mask;
            video->blue_size = vesa.blue.mask;

            log_info("video graphics %ux%u", (unsigned int)video->width, (unsigned int)video->height);

            return;
        }
    }

    log_info("video VGA text");
}
