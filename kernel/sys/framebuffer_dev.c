#include <arch/arch.h>
#include <base/units.h>
#include <errno.h>
#include <gui/fb.h>
#include <gui/pixel.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/console.h>
#include <sys/devfs.h>
#include <sys/framebuffer.h>
#include <sys/ioctl.h>
#include <sys/tty.h>

#define FB_MAP_CHUNK (4 * MIB)

static u8* _back_buf;
static size_t _back_buf_size;

static bool framebuffer_register_devfs(vfs_node_t* dev_dir);

static ssize_t _dev_fb_transfer(const framebuffer_info_t* fb, void* buf, size_t offset, size_t len, bool write) {
    if (!fb || !fb->available || !buf)
        return -1;

    u64 fb_size = fb->size;
    u64 off = offset;

    if (off >= fb_size)
        return VFS_EOF;

    u64 max_len = fb_size - off;
    u64 req = len;
    if (req > max_len)
        req = max_len;

    size_t remaining = (size_t)req;
    size_t done = 0;

    while (remaining) {
        size_t chunk = remaining;
        if (chunk > FB_MAP_CHUNK)
            chunk = FB_MAP_CHUNK;

        void* map = arch_phys_map(fb->paddr + off + done, chunk,
                                   write ? PHYS_MAP_WC : 0);
        if (!map)
            break;

        if (write)
            memcpy(map, (u8*)buf + done, chunk);
        else
            memcpy((u8*)buf + done, map, chunk);

        arch_phys_unmap(map, chunk);
        done += chunk;
        remaining -= chunk;
    }

    if (!done && req)
        return -1;

    return (ssize_t)done;
}

static ssize_t _dev_fb_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)flags;

    const framebuffer_info_t* fb = framebuffer_get_info();
    return _dev_fb_transfer(fb, buf, offset, len, false);
}

static ssize_t _dev_fb_write(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)flags;

    ssize_t owner_screen = console_fb_owner_screen();
    if (owner_screen != TTY_NONE && tty_current_screen() != (size_t)owner_screen)
        return -EAGAIN;

    const framebuffer_info_t* fb = framebuffer_get_info();
    return _dev_fb_transfer(fb, buf, offset, len, true);
}

static ssize_t _dev_fb_present(const framebuffer_info_t* fb, const void* frame) {
    if (!fb || !fb->available || !frame || !_back_buf)
        return -EINVAL;

    u32 width = fb->width;
    u32 height = fb->height;
    u32 pitch = fb->pitch;
    u32 bpp_bytes = fb->bpp / 8;
    if (!bpp_bytes)
        return -EINVAL;

    u32 row_bytes = width * bpp_bytes;
    size_t hw_frame_size = (size_t)height * row_bytes;
    const u32* src = frame;

    u8 red_shift = fb->red_shift;
    u8 green_shift = fb->green_shift;
    u8 blue_shift = fb->blue_shift;
    u8 red_size = fb->red_size;
    u8 green_size = fb->green_size;
    u8 blue_size = fb->blue_size;
    pixel_apply_legacy_defaults(
        (u8)bpp_bytes, &red_shift, &green_shift, &blue_shift, &red_size, &green_size, &blue_size
    );

    if (pixel_is_fast_bgrx8888((u8)bpp_bytes, red_shift, green_shift, blue_shift, red_size, green_size, blue_size)) {
        size_t src_row_bytes = (size_t)width * sizeof(u32);

        for (u32 y = 0; y < height; y++)
            memcpy(_back_buf + (size_t)y * row_bytes, src + (size_t)y * width, src_row_bytes);
    } else {
        for (u32 y = 0; y < height; y++) {
            const u32* src_row = src + (size_t)y * width;
            u8* dst_row = _back_buf + (size_t)y * row_bytes;

            for (u32 x = 0; x < width; x++) {
                u32 packed = pixel_pack_rgb888(
                    src_row[x], red_shift, green_shift, blue_shift, red_size, green_size, blue_size
                );
                pixel_store_packed(dst_row + (size_t)x * bpp_bytes, (u8)bpp_bytes, packed);
            }
        }
    }

    // Copy back buffer to VRAM in one shot (write-combining for speed)
    void* vram = arch_phys_map(fb->paddr, fb->size, PHYS_MAP_WC);
    if (!vram)
        return -EIO;

    if (pitch == row_bytes) {
        memcpy(vram, _back_buf, hw_frame_size);
    } else {
        for (u32 y = 0; y < height; y++)
            memcpy((u8*)vram + (size_t)y * pitch, _back_buf + (size_t)y * row_bytes, row_bytes);
    }

    arch_phys_unmap(vram, fb->size);
    return 0;
}

static ssize_t _dev_fb_ioctl(vfs_node_t* node, u64 request, void* args) {
    (void)node;

    const framebuffer_info_t* fb = framebuffer_get_info();

    switch (request) {
    case FBIOGETINFO:
        if (!args)
            return -EINVAL;

        fb_info_t* info = args;
        memset(info, 0, sizeof(*info));

        if (!fb)
            return 0;

        info->width = fb->width;
        info->height = fb->height;
        info->pitch = fb->pitch;
        info->bpp = fb->bpp;
        info->available = fb->available;

        return 0;
    case FBIOACQUIRE: {
        sched_thread_t* current = sched_current();
        if (!current)
            return -EPERM;

        if (!fb || !fb->available)
            return -ENODEV;

        size_t screen = TTY_CONSOLE;
        if (current->tty_index >= 0 && current->tty_index < TTY_SCREEN_COUNT)
            screen = (size_t)current->tty_index;

        return console_fb_acquire(current->pid, screen);
    }
    case FBIORELEASE: {
        sched_thread_t* current = sched_current();
        if (!current)
            return -EPERM;

        return console_fb_release(current->pid);
    }
    case FBIOPRESENT: {
        if (!args)
            return -EINVAL;

        ssize_t owner_screen = console_fb_owner_screen();
        if (owner_screen != TTY_NONE && tty_current_screen() != (size_t)owner_screen)
            return -EAGAIN;

        return _dev_fb_present(fb, args);
    }
    default:
        return -ENOTTY;
    }
}

void framebuffer_devfs_init(void) {
    if (!devfs_register_device("framebuffer", framebuffer_register_devfs))
        log_warn("framebuffer: failed to register devfs init callback");
}

static bool framebuffer_register_devfs(vfs_node_t* dev_dir) {
    if (!dev_dir)
        return false;

    const framebuffer_info_t* fb = framebuffer_get_info();
    if (!fb)
        return true;

    // Allocate kernel-side back buffer for double buffering
    _back_buf_size = (size_t)fb->height * (size_t)fb->width * (fb->bpp / 8);
    _back_buf = malloc(_back_buf_size);
    if (!_back_buf)
        log_warn("framebuffer: failed to allocate back buffer (%zu bytes)", _back_buf_size);

    vfs_interface_t* fb_if = vfs_create_interface(_dev_fb_read, _dev_fb_write, NULL);
    if (!fb_if) {
        log_warn("framebuffer: failed to allocate /dev interface");
        return false;
    }

    fb_if->ioctl = _dev_fb_ioctl;

    if (!devfs_register_node(dev_dir, "fb", VFS_CHARDEV, 0666, fb_if, NULL)) {
        log_warn("framebuffer: failed to create /dev/fb");
        return false;
    }

    return true;
}
