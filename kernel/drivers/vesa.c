#include "drivers/vesa.h"

#include <gfx/color.h>
#include <gfx/state.h>

#include "vfs/fs.h"

static isize _write(vfs_node* node, void* buf, usize offset, usize len) {
    if (!buf)
        return -1;

    graphics_state* gfx = node->private;
    u32* color_buf = buf;

    for (usize i = 0; i < len; i++) {
        usize x = (offset + i) % gfx->width;
        usize y = (offset + i) / gfx->width;

        vesa_draw_pixel(gfx, x, y, color_buf[i]);
    }

    return len;
}


void vesa_draw_pixel(graphics_state* gfx, usize x, usize y, u32 color) {
    u32 offset = y * gfx->pitch + x * gfx->depth;

    u32* fb = (u32*)(gfx->framebuffer + offset);

    *fb = color;
}

u32 to_vesa_color(graphics_state* gfx, rgba_color color) {
    u32 r = (color.r << gfx->red_mask);
    u32 g = (color.g << gfx->green_mask);
    u32 b = (color.b << gfx->blue_mask);

    return r | g | b;
}


void init_framebuffer(virtual_fs* vfs, graphics_state* gfx) {
    if (gfx->mode != GFX_VESA)
        return;

    vfs_node* node = vfs_create_node("fb", VFS_CHARDEV);
    node->interface = vfs_create_file_interface(NULL, _write);

    node->private = gfx;

    vfs_mount(vfs, "/dev", tree_create_node(node));
}
