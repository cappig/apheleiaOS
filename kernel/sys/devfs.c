#include "devfs.h"

#include <arch/arch.h>
#include <base/macros.h>
#include <base/units.h>
#include <errno.h>
#include <gui/fb.h>
#include <inttypes.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/cpu.h>
#include <sys/console.h>
#include <sys/framebuffer.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include "keyboard.h"
#include "input.h"
#include "mouse.h"
#include "pty.h"
#include "tty.h"
#include "vfs.h"
#include "ws.h"

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
static pty_handle_t pty_master_handles[PTY_COUNT];
static pty_handle_t pty_slave_handles[PTY_COUNT];
static pty_handle_t pty_master_default = {.index = 0, .is_master = true};
static u64 boot_seconds = 0;
static u32 ws_ids[WS_MAX_WINDOWS] = {0};

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

static short _dev_tty_poll(vfs_node_t* node, short events, u32 flags) {
    return tty_poll_handle(node ? node->private : NULL, events, flags);
}

static ssize_t _dev_pty_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)offset;

    return pty_read_handle(node ? node->private : NULL, buf, len, flags);
}

static ssize_t _dev_pty_write(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    (void)offset;

    return pty_write_handle(node ? node->private : NULL, buf, len, flags);
}

static ssize_t _dev_pty_ioctl(vfs_node_t* node, u64 request, void* args) {
    return pty_ioctl_handle(node ? node->private : NULL, request, args);
}

static short _dev_pty_poll(vfs_node_t* node, short events, u32 flags) {
    return pty_poll_handle(node ? node->private : NULL, events, flags);
}

static ssize_t _dev_input_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    return input_read(node, buf, offset, len, flags);
}

static short _dev_input_poll(vfs_node_t* node, short events, u32 flags) {
    return input_poll(node, events, flags);
}

static ssize_t _dev_wsctl_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    return ws_ctl_read(node, buf, offset, len, flags);
}

static ssize_t _dev_wsctl_write(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    return ws_ctl_write(node, buf, offset, len, flags);
}

static short _dev_wsctl_poll(vfs_node_t* node, short events, u32 flags) {
    return ws_ctl_poll(node, events, flags);
}

static ssize_t _dev_ws_fb_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    if (!node || !node->private)
        return -EINVAL;

    return ws_fb_read(*(u32*)node->private, buf, offset, len, flags);
}

static ssize_t _dev_ws_fb_write(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    if (!node || !node->private)
        return -EINVAL;

    return ws_fb_write(*(u32*)node->private, buf, offset, len, flags);
}

static short _dev_ws_fb_poll(vfs_node_t* node, short events, u32 flags) {
    if (!node || !node->private)
        return POLLNVAL;

    return ws_fb_poll(*(u32*)node->private, events, flags);
}

static ssize_t _dev_ws_ev_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
    if (!node || !node->private)
        return -EINVAL;

    return ws_ev_read(*(u32*)node->private, buf, offset, len, flags);
}

static short _dev_ws_ev_poll(vfs_node_t* node, short events, u32 flags) {
    if (!node || !node->private)
        return POLLNVAL;

    return ws_ev_poll(*(u32*)node->private, events, flags);
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

    ssize_t owner_screen = console_fb_owner_screen();
    if (owner_screen != TTY_NONE && tty_current_screen() != (size_t)owner_screen)
        return -EAGAIN;

    return dev_fb_transfer(framebuffer_get_info(), buf, offset, len, true);
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
    default:
        return -ENOTTY;
    }
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

static void _seed_pty_handles(void) {
    for (size_t i = 0; i < PTY_COUNT; i++) {
        pty_master_handles[i].index = i;
        pty_master_handles[i].is_master = true;
        pty_slave_handles[i].index = i;
        pty_slave_handles[i].is_master = false;
    }
}

static void _create_ttys(vfs_node_t* dev_dir, vfs_interface_t* tty_if) {
    char name[] = "tty0";

    for (size_t i = 0; i < TTY_COUNT; i++) {
        name[3] = (char)('0' + i);
        _create_node(dev_dir, name, VFS_CHARDEV, 0666, tty_if, &tty_handles[i]);
    }
}

static void _create_ptys(vfs_node_t* dev_dir, vfs_interface_t* pty_if) {
    char pty_name[] = "pty0";
    char pts_name[] = "pts0";

    for (size_t i = 0; i < PTY_COUNT; i++) {
        pty_name[3] = (char)('0' + i);
        pts_name[3] = (char)('0' + i);

        devfs_create_node(dev_dir, pty_name, VFS_CHARDEV, 0666, pty_if, &pty_master_handles[i]);
        devfs_create_node(dev_dir, pts_name, VFS_CHARDEV, 0666, pty_if, &pty_slave_handles[i]);
    }
}

// FIXME: this is horrible, fix this eyesore asap!
void devfs_init(void) {
    tty_init();
    pty_init();
    input_init();
    ws_init();
    devfs_seed_tty_handles();
    _seed_pty_handles();
    devfs_boot_seconds();

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

    tty_if->ioctl = dev_tty_ioctl;
    tty_if->poll = _dev_tty_poll;

    if (!_create_node(dev_dir, "tty", VFS_CHARDEV, 0666, tty_if, &tty_current))
        log_warn("devfs: failed to create /dev/tty");

    if (!_create_node(dev_dir, "console", VFS_CHARDEV, 0666, tty_if, &tty_console))
        log_warn("devfs: failed to create /dev/console");

    _create_ttys(dev_dir, tty_if);

    vfs_interface_t* pty_if = vfs_create_interface(_dev_pty_read, _dev_pty_write, NULL);
    if (!pty_if) {
        log_warn("devfs: failed to allocate pty interface");
    } else {
        pty_if->ioctl = dev_pty_ioctl;
        pty_if->poll = _dev_pty_poll;

        if (!devfs_create_node(dev_dir, "ptmx", VFS_CHARDEV, 0666, pty_if, &pty_master_default))
            log_warn("devfs: failed to create /dev/ptmx");

        _create_ptys(dev_dir, pty_if);
    }

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

    vfs_interface_t* input_if = vfs_create_interface(_dev_input_read, NULL, NULL);
    if (!input_if) {
        log_warn("devfs: failed to allocate input interface");
    } else {
        input_if->poll = _dev_input_poll;

        if (!devfs_create_node(dev_dir, "input", VFS_CHARDEV, 0666, input_if, NULL))
            log_warn("devfs: failed to create /dev/input");
    }

    vfs_interface_t* null_if = vfs_create_interface(_dev_null_read, dev_null_write, NULL);
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
        } else if (!devfs_create_node(dev_dir, "fb", VFS_CHARDEV, 0666, fb_if, NULL)) {
            log_warn("devfs: failed to create /dev/fb");
        } else {
            fb_if->ioctl = _dev_fb_ioctl;
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

    vfs_interface_t* wsctl_if = vfs_create_interface(_dev_wsctl_read, _dev_wsctl_write, NULL);
    if (!wsctl_if) {
        log_warn("devfs: failed to allocate wsctl interface");
    } else {
        wsctl_if->poll = _dev_wsctl_poll;

        if (!devfs_create_node(dev_dir, "wsctl", VFS_CHARDEV, 0666, wsctl_if, NULL))
            log_warn("devfs: failed to create /dev/wsctl");
    }

    vfs_node_t* ws_dir = vfs_lookup_from(dev_dir, "ws");
    if (!ws_dir)
        ws_dir = vfs_create(dev_dir, "ws", VFS_DIR, 0755);

    if (!ws_dir) {
        log_warn("devfs: failed to create /dev/ws");
    } else {
        vfs_interface_t* ws_fb_if = vfs_create_interface(_dev_ws_fb_read, _dev_ws_fb_write, NULL);
        vfs_interface_t* ws_ev_if = vfs_create_interface(_dev_ws_ev_read, NULL, NULL);

        if (!ws_fb_if || !ws_ev_if) {
            log_warn("devfs: failed to allocate ws window interfaces");
        } else {
            ws_fb_if->poll = _dev_ws_fb_poll;
            ws_ev_if->poll = _dev_ws_ev_poll;

            for (u32 i = 0; i < WS_MAX_WINDOWS; i++) {
                ws_ids[i] = i;

                char slot_name[4];
                snprintf(slot_name, sizeof(slot_name), "%u", i);

                vfs_node_t* slot = vfs_lookup_from(ws_dir, slot_name);
                if (!slot)
                    slot = vfs_create(ws_dir, slot_name, VFS_DIR, 0755);

                if (!slot)
                    continue;

                devfs_create_node(slot, "fb", VFS_CHARDEV, 0666, ws_fb_if, &ws_ids[i]);
                devfs_create_node(slot, "ev", VFS_CHARDEV, 0666, ws_ev_if, &ws_ids[i]);
            }
        }
    }

    log_info("devfs: devices ready");
}
