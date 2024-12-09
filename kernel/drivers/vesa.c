#include "drivers/vesa.h"

#include <gfx/color.h>
#include <gfx/state.h>
#include <string.h>

#include "arch/panic.h"
#include "vfs/fs.h"

static bool has_vesa = false;
static graphics_state gfx = {0};


static isize _write(UNUSED vfs_node* node, void* buf, usize offset, usize len) {
    if (!buf)
        return -1;

    u32* color_buf = buf;
    isize draw_counter = 0;

    for (usize i = 0; i < len; i++) {
        usize x = (offset + i) % gfx.width;
        usize y = (offset + i) / gfx.width;

        if (draw_pixel(x, y, color_buf[i]))
            draw_counter++;
    }

    return draw_counter;
}


u32 to_vesa_color(graphics_state* gfx_ptr, rgba_color color) {
    u32 r = (color.r << gfx_ptr->red_mask);
    u32 g = (color.g << gfx_ptr->green_mask);
    u32 b = (color.b << gfx_ptr->blue_mask);

    return r | g | b;
}

bool draw_pixel_rgba(usize x, usize y, rgba_color color) {
    u32 raw_color = to_vesa_color(&gfx, color);

    return draw_pixel(x, y, raw_color);
}

bool draw_pixel(usize x, usize y, u32 color) {
    assert(has_vesa);

    if (x > gfx.width || y > gfx.height)
        return false;

    u32 offset = y * gfx.pitch + x * gfx.depth;
    u32* fb = (u32*)(gfx.framebuffer + offset);

    *fb = color;

    return true;
}

void init_framebuffer(graphics_state* gfx_ptr) {
    if (gfx_ptr->mode != GFX_VESA)
        return;

    memcpy(&gfx, gfx_ptr, sizeof(graphics_state));
    has_vesa = true;
}

void init_framebuffer_dev() {
    vfs_node* node = vfs_create_node("fb", VFS_CHARDEV);
    node->interface = vfs_create_file_interface(NULL, _write);

    vfs_mount("/dev", tree_create_node(node));
}
