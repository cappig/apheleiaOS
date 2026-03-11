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
#include <sys/lock.h>
#include <sys/tty.h>
#include <sys/vfs.h>

#include "framebuffer.h"

#define FB_MAP_CHUNK (4 * MIB)
#define FB_DEV_UID   0U
#define FB_DEV_GID   44U
#define FB_DEV_MODE  0660
#define FB_PRESENT_CHUNK_BYTES (64U * 1024U)

static void *_present_vram;
static bool framebuffer_driver_loaded = false;
static mutex_t fb_present_lock = MUTEX_INIT;
static bool fb_present_force_full = true;

static void _fb_handoff_clear(const framebuffer_info_t *fb) {
    if (!fb || !fb->available || !fb->size) {
        return;
    }

    mutex_lock(&fb_present_lock);

    bool transient_map = false;
    void *vram = _present_vram;
    if (!vram) {
        vram = arch_phys_map(fb->paddr, fb->size, PHYS_MAP_WC);
        transient_map = true;
    }

    if (vram) {
        memset(vram, 0, fb->size);
        if (transient_map) {
            arch_phys_unmap(vram, fb->size);
        }
    }

    // Ensure the first frame after acquire repaints the entire display.
    fb_present_force_full = true;
    mutex_unlock(&fb_present_lock);
}

const driver_desc_t framebuffer_driver_desc = {
    .name = "framebuffer",
    .deps = NULL,
    .stage = DRIVER_STAGE_DEVFS,
    .load = framebuffer_driver_load,
    .unload = framebuffer_driver_unload,
    .is_busy = framebuffer_driver_busy,
};

static ssize_t _dev_fb_transfer(
    const framebuffer_info_t *fb,
    void *buf,
    size_t offset,
    size_t len,
    bool write
) {
    if (!fb || !fb->available || !buf) {
        return -1;
    }

    u64 fb_size = fb->size;
    u64 off = offset;

    if (off >= fb_size) {
        return VFS_EOF;
    }

    u64 max_len = fb_size - off;
    u64 req = len;
    if (req > max_len) {
        req = max_len;
    }

    size_t remaining = (size_t)req;
    size_t done = 0;

    while (remaining) {
        size_t chunk = remaining;
        if (chunk > FB_MAP_CHUNK) {
            chunk = FB_MAP_CHUNK;
        }

        void *map = arch_phys_map(
            fb->paddr + off + done, chunk, write ? PHYS_MAP_WC : 0
        );

        if (!map) {
            break;
        }

        if (write) {
            memcpy(map, (u8 *)buf + done, chunk);
        } else {
            memcpy((u8 *)buf + done, map, chunk);
        }

        arch_phys_unmap(map, chunk);
        done += chunk;
        remaining -= chunk;
    }

    if (!done && req) {
        return -1;
    }

    return (ssize_t)done;
}

static ssize_t _dev_fb_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)node;
    (void)flags;

    const framebuffer_info_t *fb = framebuffer_get_info();
    return _dev_fb_transfer(fb, buf, offset, len, false);
}

static ssize_t _dev_fb_write(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)node;
    (void)flags;

    ssize_t owner_screen = console_fb_owner_screen();
    if (owner_screen != TTY_NONE && tty_current_screen() != (size_t)owner_screen) {
        return -EAGAIN;
    }

    const framebuffer_info_t *fb = framebuffer_get_info();
    return _dev_fb_transfer(fb, buf, offset, len, true);
}

static bool _clip_present_rect(
    const framebuffer_info_t *fb,
    const fb_present_rect_t *req,
    u32 *x,
    u32 *y,
    u32 *width,
    u32 *height
) {
    if (!fb || !req || !x || !y || !width || !height) {
        return false;
    }

    if (!req->width || !req->height) {
        return false;
    }

    if (req->x >= fb->width || req->y >= fb->height) {
        return false;
    }

    *x = req->x;
    *y = req->y;
    *width = req->width;
    *height = req->height;

    if (*x + *width > fb->width) {
        *width = fb->width - *x;
    }

    if (*y + *height > fb->height) {
        *height = fb->height - *y;
    }

    return *width && *height;
}

static ssize_t _dev_fb_present_rect(
    const framebuffer_info_t *fb,
    const fb_present_rect_t *req
) {
    if (!fb || !fb->available || !req || !req->frame) {
        return -EINVAL;
    }

    u32 x = 0;
    u32 y = 0;
    u32 width = 0;
    u32 height = 0;

    if (!_clip_present_rect(fb, req, &x, &y, &width, &height)) {
        return 0;
    }

    u32 fb_width = fb->width;
    u32 pitch = fb->pitch;
    u32 bpp_bytes = fb->bpp / 8;

    if (!bpp_bytes) {
        return -EINVAL;
    }

    const u32 *src = req->frame;

    u8 red_shift = fb->red_shift;
    u8 green_shift = fb->green_shift;
    u8 blue_shift = fb->blue_shift;
    u8 red_size = fb->red_size;
    u8 green_size = fb->green_size;
    u8 blue_size = fb->blue_size;

    pixel_apply_legacy_defaults(
        (u8)bpp_bytes,
        &red_shift,
        &green_shift,
        &blue_shift,
        &red_size,
        &green_size,
        &blue_size
    );
    mutex_lock(&fb_present_lock);

    bool transient_map = false;
    void *vram = _present_vram;
    if (!vram) {
        vram = arch_phys_map(fb->paddr, fb->size, PHYS_MAP_WC);
        transient_map = true;
    }

    if (!vram) {
        mutex_unlock(&fb_present_lock);
        return -EIO;
    }

    if (fb_present_force_full) {
        x = 0;
        y = 0;
        width = fb->width;
        height = fb->height;
    }

    bool fast_bgrx = pixel_is_fast_bgrx8888(
        (u8)bpp_bytes,
        red_shift,
        green_shift,
        blue_shift,
        red_size,
        green_size,
        blue_size
    );

    if (fast_bgrx) {
        size_t src_row_bytes = (size_t)width * sizeof(u32);
        size_t chunk_rows_budget = src_row_bytes
                                     ? (FB_PRESENT_CHUNK_BYTES / src_row_bytes)
                                     : 0;
        if (!chunk_rows_budget) {
            chunk_rows_budget = 1;
        }

        for (u32 row = 0; row < height;) {
            u32 rows = height - row;
            if ((size_t)rows > chunk_rows_budget) {
                rows = (u32)chunk_rows_budget;
            }

            for (u32 r = 0; r < rows; r++) {
                const u32 *src_row = src + (size_t)(y + row + r) * fb_width + x;
                u8 *dst_row = (u8 *)vram + (size_t)(y + row + r) * pitch +
                              (size_t)x * bpp_bytes;
                memcpy(dst_row, src_row, src_row_bytes);
            }
            row += rows;
        }
    } else {
        size_t dst_row_bytes = (size_t)width * bpp_bytes;
        size_t chunk_rows_budget = dst_row_bytes
                                     ? (FB_PRESENT_CHUNK_BYTES / dst_row_bytes)
                                     : 0;
        if (!chunk_rows_budget) {
            chunk_rows_budget = 1;
        }

        for (u32 row = 0; row < height;) {
            u32 rows = height - row;
            if ((size_t)rows > chunk_rows_budget) {
                rows = (u32)chunk_rows_budget;
            }

            for (u32 r = 0; r < rows; r++) {
                const u32 *src_row = src + (size_t)(y + row + r) * fb_width + x;
                u8 *dst_row = (u8 *)vram + (size_t)(y + row + r) * pitch +
                              (size_t)x * bpp_bytes;

                for (u32 col = 0; col < width; col++) {
                    u32 packed = pixel_pack_rgb888(
                        src_row[col],
                        red_shift,
                        green_shift,
                        blue_shift,
                        red_size,
                        green_size,
                        blue_size
                    );
                    pixel_store_packed(
                        dst_row + (size_t)col * bpp_bytes, (u8)bpp_bytes, packed
                    );
                }
            }
            row += rows;
        }
    }

    fb_present_force_full = false;

    if (transient_map) {
        arch_phys_unmap(vram, fb->size);
    }

    mutex_unlock(&fb_present_lock);
    return 0;
}

static ssize_t
_dev_fb_present(const framebuffer_info_t *fb, const void *frame) {
    if (!frame) {
        return -EINVAL;
    }

    fb_present_rect_t req = {
        .frame = frame,
        .x = 0,
        .y = 0,
        .width = fb ? fb->width : 0,
        .height = fb ? fb->height : 0,
    };

    return _dev_fb_present_rect(fb, &req);
}

static ssize_t _dev_fb_ioctl(vfs_node_t *node, u64 request, void *args) {
    (void)node;

    const framebuffer_info_t *fb = framebuffer_get_info();

    switch (request) {
    case FBIOGETINFO:
        if (!args) {
            return -EINVAL;
        }

        fb_info_t *info = args;
        memset(info, 0, sizeof(*info));

        if (!fb) {
            return 0;
        }

        info->width = fb->width;
        info->height = fb->height;
        info->pitch = fb->pitch;
        info->bpp = fb->bpp;
        info->available = fb->available;

        return 0;
    case FBIOACQUIRE: {
        sched_thread_t *current = sched_current();
        if (!current) {
            return -EPERM;
        }

        if (!fb || !fb->available) {
            return -ENODEV;
        }

        size_t screen = TTY_CONSOLE;
        if (current->tty_index >= 0 && current->tty_index < TTY_SCREEN_COUNT) {
            screen = (size_t)current->tty_index;
        }

        int status = console_fb_acquire(current->pid, screen);
        if (!status) {
            _fb_handoff_clear(fb);
        }

        return status;
    }
    case FBIORELEASE: {
        sched_thread_t *current = sched_current();
        if (!current) {
            return -EPERM;
        }

        return console_fb_release(current->pid);
    }
    case FBIOPRESENT: {
        if (!args) {
            return -EINVAL;
        }

        ssize_t owner_screen = console_fb_owner_screen();
        if (owner_screen != TTY_NONE && tty_current_screen() != (size_t)owner_screen) {
            return -EAGAIN;
        }

        return _dev_fb_present(fb, args);
    }
    case FBIOPRESENT_RECT: {
        if (!args) {
            return -EINVAL;
        }

        ssize_t owner_screen = console_fb_owner_screen();
        if (owner_screen != TTY_NONE && tty_current_screen() != (size_t)owner_screen) {
            return -EAGAIN;
        }

        return _dev_fb_present_rect(fb, (const fb_present_rect_t *)args);
    }
    default:
        return -ENOTTY;
    }
}

static bool framebuffer_register_devfs(vfs_node_t *dev_dir) {
    if (!dev_dir) {
        return false;
    }

    const framebuffer_info_t *fb = framebuffer_get_info();
    if (!fb) {
        return true;
    }

    _present_vram = NULL;
    fb_present_force_full = true;
    if (arch_phys_map_can_persist()) {
        _present_vram = arch_phys_map(fb->paddr, fb->size, PHYS_MAP_WC);
        if (!_present_vram) {
            log_warn("failed to create persistent VRAM map for present path");
        }
    }

    vfs_interface_t *fb_if =
        vfs_create_interface(_dev_fb_read, _dev_fb_write, NULL);

    if (!fb_if) {
        log_warn("failed to allocate /dev interface");
        return false;
    }

    fb_if->ioctl = _dev_fb_ioctl;

    if (
        !devfs_register_node(dev_dir, "fb", VFS_CHARDEV, FB_DEV_MODE, fb_if, NULL)
    ) {
        log_warn("failed to create /dev/fb");
        return false;
    }

    vfs_node_t *fb_node = vfs_lookup("/dev/fb");
    if (!fb_node || !vfs_chown(fb_node, FB_DEV_UID, FB_DEV_GID)) {
        log_warn("failed to set /dev/fb ownership to root:video");
        return false;
    }

    return true;
}

bool framebuffer_driver_busy(void) {
    vfs_node_t *node = vfs_lookup("/dev/fb");
    return node && sched_fd_refs_node(node);
}

driver_err_t framebuffer_driver_load(void) {
    if (framebuffer_driver_loaded) {
        return DRIVER_OK;
    }

    mutex_set_name(&fb_present_lock, "fb_present_lock");

    if (!devfs_register_device("framebuffer", framebuffer_register_devfs)) {
        return DRIVER_ERR_INIT_FAILED;
    }

    framebuffer_driver_loaded = true;
    return DRIVER_OK;
}

driver_err_t framebuffer_driver_unload(void) {
    if (!framebuffer_driver_loaded) {
        return DRIVER_OK;
    }

    if (framebuffer_driver_busy()) {
        return DRIVER_ERR_BUSY;
    }

    vfs_node_t *node = vfs_lookup("/dev/fb");
    if (node && !devfs_unregister_node("/dev/fb")) {
        return DRIVER_ERR_BUSY;
    }

    if (!devfs_unregister_device("framebuffer")) {
        log_warn("failed to unregister framebuffer devfs callback");
    }

    if (_present_vram) {
        const framebuffer_info_t *fb = framebuffer_get_info();
        if (fb && fb->size) {
            arch_phys_unmap(_present_vram, fb->size);
        }
        _present_vram = NULL;
    }
    fb_present_force_full = true;

    framebuffer_driver_loaded = false;
    return DRIVER_OK;
}
