#include "drivers/vesa.h"

#include <gfx/color.h>
#include <gfx/state.h>
#include <string.h>

#include "sys/panic.h"
#include "sys/video.h"
#include "vfs/fs.h"


static isize _write(UNUSED vfs_node* node, void* buf, usize offset, usize len, u32 flags) {
    if (!buf)
        return -1;

    u32* color_buf = buf;
    isize draw_counter = 0;

    for (usize i = 0; i < len; i++) {
        usize x = (offset + i) % video.width;
        usize y = (offset + i) / video.width;

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
    u32 raw_color = to_vesa_color(&video, color);

    return draw_pixel(x, y, raw_color);
}

bool draw_pixel(usize x, usize y, u32 color) {
    assert(video.mode == GFX_VESA);

    if (x > video.width || y > video.height)
        return false;

    u32 offset = y * video.pitch + x * video.depth;
    u32* fb = (u32*)(video.framebuffer + offset);

    *fb = color;

    return true;
}


void init_framebuffer_dev(boot_handoff* handoff) {
    if (handoff->graphics.mode != GFX_VESA)
        return;

    vfs_node* node = vfs_create_node("fb", VFS_CHARDEV);
    node->interface = vfs_create_interface(NULL, _write);

    vfs_node* dev = vfs_open("/dev", VFS_DIR, true, KDIR_MODE);
    vfs_insert_child(dev, node);
}
