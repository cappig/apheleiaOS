#include "devfs.h"

#include <arch/arch.h>
#include <base/macros.h>
#include <base/units.h>
#include <log/log.h>
#include <stddef.h>
#include <string.h>
#include <sys/framebuffer.h>
#include <sys/stat.h>

#include "keyboard.h"
#include "mouse.h"
#include "tty.h"
#include "vfs.h"

#define FB_MAP_CHUNK (4 * MIB)

static tty_handle_t tty_handles[TTY_COUNT];
static tty_handle_t tty_current = {.kind = TTY_HANDLE_CURRENT, .index = 0};
static tty_handle_t tty_console = {.kind = TTY_HANDLE_CONSOLE, .index = TTY_CONSOLE};

static ssize_t _dev_tty_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)offset;
    (void)flags;

    return tty_read_handle(node ? node->private : NULL, buf, len);
}

static ssize_t _dev_tty_write(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)offset;
    (void)flags;

    return tty_write_handle(node ? node->private : NULL, buf, len);
}

static ssize_t _dev_tty_ioctl(vfs_node_t* node, u64 request, void* args) {
    return tty_ioctl_handle(node ? node->private : NULL, request, args);
}

static ssize_t _dev_null_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)buf;
    (void)offset;
    (void)len;
    (void)flags;

    return VFS_EOF;
}

static ssize_t _dev_null_write(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)buf;
    (void)offset;
    (void)flags;

    return (ssize_t)len;
}

static ssize_t _dev_zero_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)offset;
    (void)flags;

    if (!buf)
        return -1;

    memset(buf, 0, len);
    return (ssize_t)len;
}

static ssize_t _dev_zero_write(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)buf;
    (void)offset;
    (void)flags;

    return (ssize_t)len;
}

static ssize_t
_dev_fb_transfer(const framebuffer_info_t* fb, void* buf, size_t offset, size_t len, bool write) {
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

        void* map = arch_phys_map(fb->paddr + off + done, chunk);
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

    if (!done)
        return -1;

    return (ssize_t)done;
}

static ssize_t _dev_fb_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)flags;

    return _dev_fb_transfer(framebuffer_get_info(), buf, offset, len, false);
}

static ssize_t _dev_fb_write(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)flags;

    return _dev_fb_transfer(framebuffer_get_info(), buf, offset, len, true);
}

static bool _create_node(
    vfs_node_t* parent,
    const char* name,
    u32 type,
    mode_t mode,
    vfs_interface_t* interface,
    void* priv
) {
    vfs_node_t* node = vfs_lookup_from(parent, name);

    if (!node)
        node = vfs_create(parent, (char*)name, type, mode);

    if (!node)
        return false;

    node->type = type;
    node->mode = mode;
    node->interface = interface;
    node->private = priv;
    return true;
}

static void _seed_tty_handles(void) {
    for (size_t i = 0; i < TTY_COUNT; i++) {
        tty_handles[i].kind = TTY_HANDLE_NAMED;
        tty_handles[i].index = i;
    }
}

static void _create_ttys(vfs_node_t* dev_dir, vfs_interface_t* tty_if) {
    char name[] = "tty0";

    for (size_t i = 0; i < TTY_COUNT; i++) {
        name[3] = (char)('0' + i);
        devfs_create_node(dev_dir, name, VFS_CHARDEV, 0666, tty_if, &tty_handles[i]);
    }
}

void devfs_init(void) {
    tty_init();
    _seed_tty_handles();

    vfs_node_t* root = vfs_lookup("/");

    if (!root) {
        log_warn("devfs: missing root");
        return;
    }

    vfs_node_t* dev_dir = vfs_lookup("/dev");

    if (!dev_dir)
        dev_dir = vfs_create(root, "dev", VFS_DIR, 0755);

    if (!dev_dir) {
        log_warn("devfs: failed to create /dev");
        return;
    }

    vfs_interface_t* tty_if = vfs_create_interface(dev_tty_read, dev_tty_write, NULL);

    if (!tty_if) {
        log_warn("devfs: failed to allocate tty interface");
        return;
    }

    tty_if->ioctl = _dev_tty_ioctl;

    if (!devfs_create_node(dev_dir, "tty", VFS_CHARDEV, 0666, tty_if, &tty_current))
        log_warn("devfs: failed to create /dev/tty");

    if (!_create_node(dev_dir, "console", VFS_CHARDEV, 0666, tty_if, &tty_console))
        log_warn("devfs: failed to create /dev/console");

    _create_ttys(dev_dir, tty_if);

    if (!keyboard_init())
        log_warn("devfs: keyboard init failed");
    vfs_interface_t* kbd_if = vfs_create_interface(keyboard_read, NULL, NULL);
    if (!kbd_if) {
        log_warn("devfs: failed to allocate keyboard interface");
    } else if (!devfs_create_node(dev_dir, "kbd", VFS_CHARDEV, 0666, kbd_if, NULL)) {
        log_warn("devfs: failed to create /dev/kbd");
    }

    if (!mouse_init())
        log_warn("devfs: mouse init failed");
    vfs_interface_t* mouse_if = vfs_create_interface(mouse_read, NULL, NULL);
    if (!mouse_if) {
        log_warn("devfs: failed to allocate mouse interface");
    } else if (!devfs_create_node(dev_dir, "mouse", VFS_CHARDEV, 0666, mouse_if, NULL)) {
        log_warn("devfs: failed to create /dev/mouse");
    }

    vfs_interface_t* null_if = vfs_create_interface(dev_null_read, dev_null_write, NULL);
    if (!null_if) {
        log_warn("devfs: failed to allocate null interface");
    } else if (!_create_node(dev_dir, "null", VFS_CHARDEV, 0666, null_if, NULL)) {
        log_warn("devfs: failed to create /dev/null");
    }

    vfs_interface_t* zero_if = vfs_create_interface(dev_zero_read, dev_zero_write, NULL);
    if (!zero_if) {
        log_warn("devfs: failed to allocate zero interface");
    } else if (!_create_node(dev_dir, "zero", VFS_CHARDEV, 0666, zero_if, NULL)) {
        log_warn("devfs: failed to create /dev/zero");
    }

    const framebuffer_info_t* fb = framebuffer_get_info();
    if (fb) {
        vfs_interface_t* fb_if = vfs_create_interface(dev_fb_read, dev_fb_write, NULL);
        if (!fb_if) {
            log_warn("devfs: failed to allocate framebuffer interface");
        } else if (!_create_node(dev_dir, "fb", VFS_CHARDEV, 0660, fb_if, NULL)) {
            log_warn("devfs: failed to create /dev/fb");
        }
    }

    log_info("devfs: devices ready");
}
