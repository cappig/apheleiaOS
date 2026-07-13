#include "framebuffer.h"

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
#include <sys/usercopy.h>
#include <sys/vfs.h>

#define FB_MAP_CHUNK        (4 * MIB)
#define FB_DEV_UID          0U
#define FB_DEV_GID          44U
#define FB_DEV_MODE         0660
#define PRESENT_CHUNK_BYTES (64U * 1024U)

typedef struct {
    void *present_vram;
    bool loaded;
    mutex_t present_lock;
    bool force_full_present;
} fb_state_t;

typedef struct {
    u32 x;
    u32 y;
    u32 width;
    u32 height;
    u32 fb_width;
    u32 vram_y;
    u32 pitch;
    u32 bpp_bytes;
    pixel_format_t fmt;
    const u32 *src;
    void *vram;
} fb_present_t;

static fb_state_t fb_driver = {
    .present_lock = MUTEX_INIT,
    .force_full_present = true,
};

static size_t _chunk_rows(size_t row_bytes) {
    if (!row_bytes) {
        return 1;
    }

    size_t rows = PRESENT_CHUNK_BYTES / row_bytes;
    if (!rows) {
        return 1;
    }

    return rows;
}

static void _fb_handoff_begin(void) {
    mutex_lock(&fb_driver.present_lock);
    fb_driver.force_full_present = true;
    mutex_unlock(&fb_driver.present_lock);
}

static ssize_t _dev_fb_transfer(const framebuffer_info_t *fb, void *buf, size_t offset, size_t len, bool write) {
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

        void *map = arch_phys_map(fb->paddr + off + done, chunk, write ? PHYS_MAP_WC : 0);

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

static ssize_t _dev_fb_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)flags;

    const framebuffer_info_t *fb = framebuffer_get_info();
    return _dev_fb_transfer(fb, buf, offset, len, false);
}

static ssize_t _dev_fb_write(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
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

    u32 max_width = fb->width - *x;
    if (*width > max_width) {
        *width = max_width;
    }

    u32 max_height = fb->height - *y;
    if (*height > max_height) {
        *height = max_height;
    }

    return *width && *height;
}

static bool _fb_frame_ok(const framebuffer_info_t *fb, const pixel_t *frame, u32 x, u32 y, u32 width, u32 height) {
    if (!fb || !fb->available || !frame || !width || !height) {
        return false;
    }

    if (x >= fb->width || y >= fb->height || width > fb->width - x || height > fb->height - y) {
        return false;
    }

    size_t max_size = (size_t)-1;
    if ((size_t)y > max_size / fb->width || (size_t)(height - 1U) > max_size / fb->width) {
        return false;
    }

    size_t first_row = (size_t)y * fb->width;
    size_t span_rows = (size_t)(height - 1U) * fb->width;
    if (first_row > max_size - x || span_rows > max_size - width) {
        return false;
    }

    size_t first = first_row + x;
    size_t span = span_rows + width;
    if (first > (size_t)-1 / sizeof(pixel_t) || span > (size_t)-1 / sizeof(pixel_t)) {
        return false;
    }

    size_t byte_offset = first * sizeof(pixel_t);
    uintptr_t base = (uintptr_t)frame;
    if (base > UINTPTR_MAX - byte_offset) {
        return false;
    }

    const void *start = (const void *)(base + byte_offset);
    sched_thread_t *current = sched_current();
    return user_range_ok(current, start, span * sizeof(pixel_t), false);
}

static void present_copy_fast(const fb_present_t *present) {
    size_t row_bytes = (size_t)present->width * sizeof(u32);
    size_t rows_budget = _chunk_rows(row_bytes);

    for (u32 row = 0; row < present->height;) {
        u32 rows = present->height - row;
        if ((size_t)rows > rows_budget) {
            rows = (u32)rows_budget;
        }

        for (u32 r = 0; r < rows; r++) {
            u32 src_y = present->y + row + r;
            const u32 *src = present->src + (size_t)src_y * present->fb_width + present->x;
            size_t dst_y = (size_t)(src_y - present->vram_y);
            u8 *dst = (u8 *)present->vram + dst_y * present->pitch + (size_t)present->x * present->bpp_bytes;

            memcpy(dst, src, row_bytes);
        }

        row += rows;
    }
}

static void present_copy_convert(const fb_present_t *present) {
    size_t row_bytes = (size_t)present->width * present->bpp_bytes;
    size_t rows_budget = _chunk_rows(row_bytes);

    for (u32 row = 0; row < present->height;) {
        u32 rows = present->height - row;
        if ((size_t)rows > rows_budget) {
            rows = (u32)rows_budget;
        }

        for (u32 r = 0; r < rows; r++) {
            u32 src_y = present->y + row + r;
            const u32 *src = present->src + (size_t)src_y * present->fb_width + present->x;
            size_t dst_y = (size_t)(src_y - present->vram_y);
            u8 *dst = (u8 *)present->vram + dst_y * present->pitch + (size_t)present->x * present->bpp_bytes;

            for (u32 col = 0; col < present->width; col++) {
                u32 packed = pixel_pack_rgb888(src[col], &present->fmt);
                pixel_store_packed(dst + (size_t)col * present->bpp_bytes, (u8)present->bpp_bytes, packed);
            }
        }

        row += rows;
    }
}

static ssize_t present_rect(const framebuffer_info_t *fb, const fb_present_rect_t *req) {
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

    u32 bpp_bytes = fb->bpp / 8;

    if (!bpp_bytes || !fb->pitch) {
        return -EINVAL;
    }

    pixel_format_t fmt = {
        .bytes_per_pixel = (u8)bpp_bytes,
        .red_shift = fb->red_shift,
        .green_shift = fb->green_shift,
        .blue_shift = fb->blue_shift,
        .red_size = fb->red_size,
        .green_size = fb->green_size,
        .blue_size = fb->blue_size,
    };
    pixel_fill_rgb_defaults(&fmt);

    mutex_lock(&fb_driver.present_lock);

    if (fb_driver.force_full_present) {
        x = 0;
        y = 0;
        width = fb->width;
        height = fb->height;
    }

    if (!_fb_frame_ok(fb, req->frame, x, y, width, height)) {
        mutex_unlock(&fb_driver.present_lock);
        return -EFAULT;
    }

    bool transient_map = false;
    void *vram = fb_driver.present_vram;
    size_t map_size = fb->size;
    u32 vram_y = 0;

    if (!vram) {
        if ((size_t)y > (size_t)-1 / fb->pitch || height > (size_t)-1 / fb->pitch) {
            mutex_unlock(&fb_driver.present_lock);
            return -EOVERFLOW;
        }

        size_t map_offset = (size_t)y * fb->pitch;
        map_size = (size_t)height * fb->pitch;
        if (map_offset > fb->size || map_size > fb->size - map_offset) {
            mutex_unlock(&fb_driver.present_lock);
            return -EOVERFLOW;
        }

        if ((u64)map_offset > UINT64_MAX - fb->paddr) {
            mutex_unlock(&fb_driver.present_lock);
            return -EOVERFLOW;
        }

        vram = arch_phys_map(fb->paddr + map_offset, map_size, PHYS_MAP_WC);
        transient_map = true;
        vram_y = y;
    }

    if (!vram) {
        mutex_unlock(&fb_driver.present_lock);
        return -EIO;
    }

    fb_present_t present = {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
        .fb_width = fb->width,
        .vram_y = vram_y,
        .pitch = fb->pitch,
        .bpp_bytes = bpp_bytes,
        .fmt = fmt,
        .src = req->frame,
        .vram = vram,
    };

    bool fast_bgrx = pixel_is_fast_bgrx8888(&fmt);

    if (fast_bgrx) {
        present_copy_fast(&present);
    } else {
        present_copy_convert(&present);
    }

    __sync_synchronize();

    fb_driver.force_full_present = false;

    if (transient_map) {
        arch_phys_unmap(vram, map_size);
    }

    mutex_unlock(&fb_driver.present_lock);
    return 0;
}

static ssize_t _dev_fb_present(const framebuffer_info_t *fb, const void *frame) {
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

    return present_rect(fb, &req);
}

static ssize_t _dev_fb_ioctl(vfs_node_t *node, u64 request, void *args) {
    (void)node;

    const framebuffer_info_t *fb = framebuffer_get_info();

    switch (request) {
    case FBIOGETINFO: {
        if (!args) {
            return -EINVAL;
        }

        fb_info_t info = { 0 };

        if (!fb) {
            sched_thread_t *current = sched_current();
            if (!user_copy_to(current, args, &info, sizeof(info))) {
                return -EFAULT;
            }

            return 0;
        }

        info.width = fb->width;
        info.height = fb->height;
        info.pitch = fb->pitch;
        info.bpp = fb->bpp;
        info.available = fb->available;

        sched_thread_t *current = sched_current();
        if (!user_copy_to(current, args, &info, sizeof(info))) {
            return -EFAULT;
        }

        return 0;
    }
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
            _fb_handoff_begin();
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

        sched_thread_t *current = sched_current();
        fb_present_rect_t req = { 0 };
        if (!user_copy_from(current, &req, args, sizeof(req))) {
            return -EFAULT;
        }

        ssize_t owner_screen = console_fb_owner_screen();
        if (owner_screen != TTY_NONE && tty_current_screen() != (size_t)owner_screen) {
            return -EAGAIN;
        }

        return present_rect(fb, &req);
    }
    default:
        return -ENOTTY;
    }
}

static bool _register_devfs(vfs_node_t *dev_dir) {
    if (!dev_dir) {
        return false;
    }

    const framebuffer_info_t *fb = framebuffer_get_info();
    if (!fb) {
        return true;
    }

    fb_driver.present_vram = NULL;
    fb_driver.force_full_present = true;
    if (arch_keeps_phys_map()) {
        fb_driver.present_vram = arch_phys_map(fb->paddr, fb->size, PHYS_MAP_WC);
        if (!fb_driver.present_vram) {
            log_warn("failed to create persistent VRAM map for present path");
        }
    }

    vfs_interface_t *fb_if = vfs_create_interface(_dev_fb_read, _dev_fb_write, NULL);

    if (!fb_if) {
        log_warn("failed to allocate /dev interface");
        return false;
    }

    fb_if->ioctl = _dev_fb_ioctl;

    if (!devfs_register_node(dev_dir, "fb", VFS_CHARDEV, FB_DEV_MODE, fb_if, NULL)) {
        log_warn("failed to create /dev/fb");
        return false;
    }

    vfs_node_t *fb_node = vfs_lookup("/dev/fb");
    if (!fb_node || vfs_chown(fb_node, FB_DEV_UID, FB_DEV_GID) < 0) {
        log_warn("failed to set /dev/fb ownership to root:video");
        return false;
    }

    return true;
}

static bool fb_busy(void) {
    vfs_node_t *node = vfs_lookup("/dev/fb");
    return node && sched_fd_refs_node(node);
}

static driver_err_t fb_load(void) {
    if (fb_driver.loaded) {
        return DRIVER_OK;
    }

    if (!devfs_register_device("framebuffer", _register_devfs)) {
        return DRIVER_ERR_INIT_FAILED;
    }

    fb_driver.loaded = true;
    return DRIVER_OK;
}

static driver_err_t fb_unload(void) {
    if (!fb_driver.loaded) {
        return DRIVER_OK;
    }

    if (fb_busy()) {
        return DRIVER_ERR_BUSY;
    }

    vfs_node_t *node = vfs_lookup("/dev/fb");
    if (node && !devfs_unregister_node("/dev/fb")) {
        return DRIVER_ERR_BUSY;
    }

    if (!devfs_unregister_device("framebuffer")) {
        log_warn("failed to unregister framebuffer devfs callback");
    }

    if (fb_driver.present_vram) {
        const framebuffer_info_t *fb = framebuffer_get_info();
        if (fb && fb->size) {
            arch_phys_unmap(fb_driver.present_vram, fb->size);
        }
        fb_driver.present_vram = NULL;
    }
    fb_driver.force_full_present = true;

    fb_driver.loaded = false;
    return DRIVER_OK;
}

const driver_desc_t framebuffer_driver_desc = {
    .name = "framebuffer",
    .deps = NULL,
    .stage = DRIVER_STAGE_DEVFS,
    .load = fb_load,
    .unload = fb_unload,
    .is_busy = fb_busy,
};
