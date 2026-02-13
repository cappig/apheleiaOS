#include "devfs.h"

#include <arch/arch.h>
#include <base/macros.h>
#include <base/units.h>
#include <inttypes.h>
#include <log/log.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/cpu.h>
#include <sys/framebuffer.h>
#include <sys/stat.h>

#include "keyboard.h"
#include "mouse.h"
#include "tty.h"
#include "vfs.h"

#define FB_MAP_CHUNK (4 * MIB)
#define SYSINFO_TEXT_MAX 384

#ifndef BUILD_DATE
#define BUILD_DATE "unknown"
#endif

#ifndef GIT_COMMIT
#define GIT_COMMIT "unknown"
#endif

static tty_handle_t tty_handles[TTY_COUNT];
static tty_handle_t tty_current = {.kind = TTY_HANDLE_CURRENT, .index = 0};
static tty_handle_t tty_console = {.kind = TTY_HANDLE_CONSOLE, .index = TTY_CONSOLE};
static u64 boot_seconds = 0;

static u64 _boot_seconds(void) {
    if (boot_seconds)
        return boot_seconds;

    u64 now = arch_wallclock_seconds();
    u32 hz = arch_timer_hz();

    if (!hz) {
        boot_seconds = now;
        return boot_seconds;
    }

    u64 uptime = arch_timer_ticks() / hz;

    if (now > uptime)
        boot_seconds = now - uptime;
    else
        boot_seconds = 0;

    return boot_seconds;
}

static ssize_t _dev_text_read(const char* text, void* buf, size_t offset, size_t len) {
    if (!text || !buf)
        return -1;

    size_t text_len = strlen(text);

    if (offset >= text_len)
        return VFS_EOF;

    size_t copy_len = text_len - offset;

    if (copy_len > len)
        copy_len = len;

    memcpy(buf, text + offset, copy_len);
    return (ssize_t)copy_len;
}

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

static ssize_t _dev_os_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)flags;

    char text[SYSINFO_TEXT_MAX];
    snprintf(
        text,
        sizeof(text),
        "name=apheleiaOS\n"
        "release=pre-alpha\n"
        "version=%s %s\n"
        "arch=%s\n",
        BUILD_DATE,
        GIT_COMMIT,
        arch_name()
    );

    return _dev_text_read(text, buf, offset, len);
}

static ssize_t _dev_cpu_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)flags;

    char text[SYSINFO_TEXT_MAX];
    snprintf(
        text,
        sizeof(text),
        "model=%s\n"
        "ncpu=%zu\n"
        "pagesize=4096\n"
        "clockrate_khz=%" PRIu64 "\n",
        arch_cpu_name(),
        core_count,
        arch_cpu_khz()
    );

    return _dev_text_read(text, buf, offset, len);
}

static ssize_t _dev_clock_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)flags;

    u64 now = arch_wallclock_seconds();
    u64 boot = _boot_seconds();
    u64 hz = arch_timer_hz();
    u64 ticks = arch_timer_ticks();

    char text[SYSINFO_TEXT_MAX];
    snprintf(
        text,
        sizeof(text),
        "now=%" PRIu64 "\n"
        "boot=%" PRIu64 "\n"
        "hz=%" PRIu64 "\n"
        "ticks=%" PRIu64 "\n",
        now,
        boot,
        hz,
        ticks
    );

    return _dev_text_read(text, buf, offset, len);
}

static ssize_t _dev_swap_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)flags;

    u64 total_kib = (u64)arch_mem_total() / KIB;
    u64 free_kib = (u64)arch_mem_free() / KIB;
    u64 used_kib = total_kib >= free_kib ? total_kib - free_kib : 0;

    char text[SYSINFO_TEXT_MAX];
    snprintf(
        text,
        sizeof(text),
        "total_kib=%" PRIu64 "\n"
        "used_kib=%" PRIu64 "\n",
        total_kib,
        used_kib
    );

    return _dev_text_read(text, buf, offset, len);
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
        _create_node(dev_dir, name, VFS_CHARDEV, 0666, tty_if, &tty_handles[i]);
    }
}

void devfs_init(void) {
    tty_init();
    devfs_seed_tty_handles();
    _boot_seconds();

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

    if (!_create_node(dev_dir, "tty", VFS_CHARDEV, 0666, tty_if, &tty_current))
        log_warn("devfs: failed to create /dev/tty");

    if (!_create_node(dev_dir, "console", VFS_CHARDEV, 0666, tty_if, &tty_console))
        log_warn("devfs: failed to create /dev/console");

    _create_ttys(dev_dir, tty_if);

    if (!keyboard_init())
        log_warn("devfs: keyboard init failed");
    vfs_interface_t* kbd_if = vfs_create_interface(keyboard_read, NULL, NULL);
    if (!kbd_if) {
        log_warn("devfs: failed to allocate keyboard interface");
    } else if (!_create_node(dev_dir, "kbd", VFS_CHARDEV, 0666, kbd_if, NULL)) {
        log_warn("devfs: failed to create /dev/kbd");
    }

    if (!mouse_init())
        log_warn("devfs: mouse init failed");
    vfs_interface_t* mouse_if = vfs_create_interface(mouse_read, NULL, NULL);
    if (!mouse_if) {
        log_warn("devfs: failed to allocate mouse interface");
    } else if (!_create_node(dev_dir, "mouse", VFS_CHARDEV, 0666, mouse_if, NULL)) {
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

    vfs_interface_t* os_if = vfs_create_interface(_dev_os_read, NULL, NULL);
    if (!os_if) {
        log_warn("devfs: failed to allocate os interface");
    } else if (!_create_node(dev_dir, "os", VFS_CHARDEV, 0444, os_if, NULL)) {
        log_warn("devfs: failed to create /dev/os");
    }

    vfs_interface_t* clock_if = vfs_create_interface(_dev_clock_read, NULL, NULL);
    if (!clock_if) {
        log_warn("devfs: failed to allocate clock interface");
    } else if (!_create_node(dev_dir, "clock", VFS_CHARDEV, 0444, clock_if, NULL)) {
        log_warn("devfs: failed to create /dev/clock");
    }

    vfs_interface_t* swap_if = vfs_create_interface(_dev_swap_read, NULL, NULL);
    if (!swap_if) {
        log_warn("devfs: failed to allocate swap interface");
    } else if (!_create_node(dev_dir, "swap", VFS_CHARDEV, 0444, swap_if, NULL)) {
        log_warn("devfs: failed to create /dev/swap");
    }

    vfs_interface_t* cpu_if = vfs_create_interface(_dev_cpu_read, NULL, NULL);
    if (!cpu_if) {
        log_warn("devfs: failed to allocate cpu interface");
    } else if (!_create_node(dev_dir, "cpu", VFS_CHARDEV, 0444, cpu_if, NULL)) {
        log_warn("devfs: failed to create /dev/cpu");
    }

    log_info("devfs: devices ready");
}
