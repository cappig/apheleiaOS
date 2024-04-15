#include "vesa.h"

#include <base/addr.h>
#include <base/macros.h>
#include <boot/proto.h>
#include <gfx/vga.h>
#include <x86/regs.h>

#include "bios.h"
#include "tty.h"


static bool _fetch_vbe_info(vesa_info* buffer) {
    regs r = {0};
    r.ax = 0x4f00;
    r.edi = (u32)(uptr)buffer;

    bios_call(0x10, &r, &r);

    return (r.ax != 0x4f);
}

static bool _fetch_mode_info(vesa_mode* buffer, u16 mode) {
    regs r = {0};
    r.ax = 0x4f01;
    r.cx = mode;
    r.edi = (u32)(uptr)buffer;

    bios_call(0x10, &r, &r);

    return (r.ax != 0x4f);
}

static void _set_vesa_mode(u16 mode_index) {
    regs r = {0};
    r.ax = 0x4f02;
    r.bx = mode_index | (1 << 14); // use linear framebuffer

    bios_call(0x10, &r, &r);

    if (r.ax != 0x4f)
        panic("Failed to set VESA mode!");
}

// Find the mode with the highest possible resolution and bpp
// If a fitting mode was found it sets it up
static vesa_mode _init_vesa(u16 max_width, u16 max_height, u16 max_bpp) {
    vesa_info info_buffer = {0};
    vesa_mode current_mode = {0}, best_mode = {0};

    if (_fetch_vbe_info(&info_buffer))
        return best_mode;

    u16* mode_ptr = (u16*)(uptr)SEG_TO_PTR(info_buffer.video_mode.seg, info_buffer.video_mode.off);

    usize best_mode_i = 0;
    for (usize i = 0; mode_ptr[i] != 0xffff; i++) {
        if (_fetch_mode_info(&current_mode, mode_ptr[i]))
            continue;

        // Check if mode is direct color
        if (current_mode.memory_model != 0x06)
            continue;

        // Check if mode supports linear frame buffer
        if ((current_mode.attributes & 0x90) != 0x90)
            continue;

        // Ignore modes that are too large
        if (current_mode.width > max_width || current_mode.height > max_height ||
            current_mode.bits_per_pixel > max_bpp) {
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


void init_graphics(graphics_state* gfx, u8 mode, u16 width, u16 height, u16 bpp) {
    if (mode == GFX_VESA) {
        vesa_mode vesa = _init_vesa(width, height, bpp);

        if (vesa.bits_per_pixel) {
            gfx->mode = GFX_VESA;

            gfx->framebuffer = (u64)(uptr)vesa.framebuffer;

            gfx->width = vesa.width;
            gfx->height = vesa.height;

            gfx->depth = (u32)vesa.bits_per_pixel / 8;
            gfx->pitch = (u32)vesa.bytes_per_line;

            return;
        }
    }

    gfx->mode = GFX_VGA;

    gfx->framebuffer = VGA_ADDR;

    gfx->width = VGA_WIDTH;
    gfx->height = VGA_HEIGHT;
}
