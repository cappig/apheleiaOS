#include "devfs.h"

#include <arch/arch.h>
#include <base/units.h>
#include <data/vector.h>
#include <errno.h>
#include <inttypes.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cpu.h>
#include <sys/stats.h>
#include <sys/stat.h>

#include "vfs.h"

#define SYSINFO_TEXT_MAX  384

#ifndef BUILD_DATE
#define BUILD_DATE "unknown"
#endif

#ifndef GIT_COMMIT
#define GIT_COMMIT "unknown"
#endif

#ifndef VERSION
#define VERSION "unknown"
#endif

static u64 boot_seconds = 0;

typedef struct {
    const char *name;
    devfs_device_init_fn init_fn;
} devfs_device_entry_t;

static vector_t *devfs_devices = NULL;
static bool devfs_ready = false;

static u64 _boot_seconds(void) {
    if (boot_seconds) {
        return boot_seconds;
    }

    u64 now = arch_wallclock_seconds();
    u32 hz = arch_timer_hz();

    if (!hz) {
        boot_seconds = now;
        return boot_seconds;
    }

    u64 uptime = arch_timer_ticks() / hz;

    if (now > uptime) {
        boot_seconds = now - uptime;
    } else {
        boot_seconds = 0;
    }

    return boot_seconds;
}

static ssize_t
_dev_text_read(const char *text, void *buf, size_t offset, size_t len) {
    if (!text || !buf) {
        return -1;
    }

    size_t text_len = strlen(text);

    if (offset >= text_len) {
        return VFS_EOF;
    }

    size_t copy_len = text_len - offset;
    if (copy_len > len) {
        copy_len = len;
    }

    memcpy(buf, text + offset, copy_len);
    return (ssize_t)copy_len;
}

static ssize_t _dev_null_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)node;
    (void)buf;
    (void)offset;
    (void)len;
    (void)flags;

    return VFS_EOF;
}

static ssize_t _dev_null_write(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)node;
    (void)buf;
    (void)offset;
    (void)flags;

    return (ssize_t)len;
}

static ssize_t _dev_zero_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)node;
    (void)offset;
    (void)flags;

    if (!buf) {
        return -1;
    }

    memset(buf, 0, len);
    return (ssize_t)len;
}

static ssize_t _dev_zero_write(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)node;
    (void)buf;
    (void)offset;
    (void)flags;

    return (ssize_t)len;
}

static ssize_t _dev_os_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)node;
    (void)flags;

    char text[SYSINFO_TEXT_MAX];
    snprintf(
        text,
        sizeof(text),
        "name=apheleiaOS\n"
        "release=" VERSION "\n"
        "version=%s %s\n"
        "arch=%s\n",
        BUILD_DATE,
        GIT_COMMIT,
        arch_name()
    );

    return _dev_text_read(text, buf, offset, len);
}

static ssize_t _dev_cpu_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)node;
    (void)flags;

    u64 busy_ticks = 0;
    u64 total_ticks = 0;
    sched_cpu_usage_snapshot(&busy_ticks, &total_ticks);

    if (busy_ticks > total_ticks) {
        busy_ticks = total_ticks;
    }

    u64 idle_ticks = total_ticks - busy_ticks;

    size_t ncpu = core_online_count ? core_online_count : 1;
    if (ncpu > MAX_CORES) {
        ncpu = MAX_CORES;
    }

    char text[SYSINFO_TEXT_MAX * 24];
    size_t used = 0;
    int wrote = snprintf(
        text + used,
        sizeof(text) - used,
        "model=%s\n"
        "ncpu=%zu\n"
        "pagesize=4096\n"
        "clockrate_khz=%" PRIu64 "\n"
        "busy_ticks=%" PRIu64 "\n"
        "idle_ticks=%" PRIu64 "\n"
        "total_ticks=%" PRIu64 "\n",
        arch_cpu_name(),
        ncpu,
        arch_cpu_khz(),
        busy_ticks,
        idle_ticks,
        total_ticks
    );

    if (wrote <= 0 || (size_t)wrote >= sizeof(text) - used) {
        return _dev_text_read(text, buf, offset, len);
    }

    used += (size_t)wrote;

    for (size_t i = 0; i < ncpu; i++) {
        u64 core_busy = 0;
        u64 core_total = 0;
        sched_cpu_usage_snapshot_core(i, &core_busy, &core_total);
        if (core_busy > core_total) {
            core_busy = core_total;
        }
        u64 core_idle = core_total - core_busy;

        wrote = snprintf(
            text + used,
            sizeof(text) - used,
            "core%zu_busy_ticks=%" PRIu64 "\n"
            "core%zu_idle_ticks=%" PRIu64 "\n"
            "core%zu_total_ticks=%" PRIu64 "\n",
            i,
            core_busy,
            i,
            core_idle,
            i,
            core_total
        );

        if (wrote <= 0 || (size_t)wrote >= sizeof(text) - used) {
            break;
        }

        used += (size_t)wrote;
    }

    return _dev_text_read(text, buf, offset, len);
}

static ssize_t _dev_clock_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)node;
    (void)flags;

    stats_snapshot_t snapshot = {0};
    stats_take_snapshot(&snapshot);

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
        "ticks=%" PRIu64 "\n"
        "timer_irq_ns=%" PRIu64 "\n",
        now,
        boot,
        hz,
        ticks,
        snapshot.timer_irq_ns
    );

    return _dev_text_read(text, buf, offset, len);
}

static ssize_t _dev_swap_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
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

static ssize_t _dev_stats_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)node;
    (void)flags;

    stats_snapshot_t snapshot = {0};
    stats_take_snapshot(&snapshot);

    char text[SYSINFO_TEXT_MAX * 3];
    snprintf(
        text,
        sizeof(text),
        "sched_switch_count=%" PRIu64 "\n"
        "poll_sleep_loops=%" PRIu64 "\n"
        "ws_fb_write_bytes=%" PRIu64 "\n"
        "fb_present_bytes=%" PRIu64 "\n"
        "wm_dirty_pixels=%" PRIu64 "\n",
        snapshot.sched_switch_count,
        snapshot.poll_sleep_loops,
        snapshot.ws_fb_write_bytes,
        snapshot.fb_present_bytes,
        snapshot.wm_dirty_pixels
    );

    return _dev_text_read(text, buf, offset, len);
}

bool devfs_register_node(
    vfs_node_t *parent,
    const char *name,
    u32 type,
    mode_t mode,
    vfs_interface_t *interface,
    void *priv
) {
    if (!parent || !name) {
        return false;
    }

    vfs_node_t *node = vfs_lookup_from(parent, name);

    if (!node) {
        node = vfs_create_virtual(parent, (char *)name, type, mode);
    }

    if (!node) {
        return false;
    }

    node->type = type;
    node->mode = mode;
    node->interface = interface;
    node->private = priv;

    return true;
}

vfs_node_t *
devfs_register_dir(vfs_node_t *parent, const char *name, mode_t mode) {
    if (!parent || !name) {
        return NULL;
    }

    vfs_node_t *node = vfs_lookup_from(parent, name);
    if (!node) {
        node = vfs_create_virtual(parent, (char *)name, VFS_DIR, mode);
    }

    if (!node) {
        return NULL;
    }

    node->type = VFS_DIR;
    node->mode = mode;
    node->interface = NULL;
    node->private = NULL;

    return node;
}

static vfs_node_t *_ensure_dev_dir(void) {
    vfs_node_t *root = vfs_lookup("/");
    if (!root) {
        log_warn("missing root");
        return NULL;
    }

    vfs_node_t *dev_dir = vfs_lookup("/dev");
    if (!dev_dir) {
        dev_dir = vfs_create_virtual(root, "dev", VFS_DIR, 0755);
    }

    if (!dev_dir) {
        log_warn("failed to create /dev");
        return NULL;
    }

    // Keep /dev purely in-memory even if a backing fs is mounted
    if (dev_dir->interface) {
        free(dev_dir->interface);
        dev_dir->interface = NULL;
    }

    dev_dir->type = VFS_DIR;
    dev_dir->mode = 0755;

    return dev_dir;
}

static bool _ensure_device_registry(void) {
    if (devfs_devices) {
        return true;
    }

    devfs_devices = vec_create(sizeof(devfs_device_entry_t));
    if (!devfs_devices) {
        log_warn("failed to allocate devfs device registry");
        return false;
    }

    return true;
}

bool devfs_register_device(const char *name, devfs_device_init_fn init_fn) {
    if (!name || !init_fn) {
        return false;
    }

    if (!_ensure_device_registry()) {
        return false;
    }

    for (size_t i = 0; i < devfs_devices->size; i++) {
        devfs_device_entry_t *entry = vec_at(devfs_devices, i);
        if (entry && entry->init_fn == init_fn) {
            return true;
        }
    }

    devfs_device_entry_t entry = {
        .name = name,
        .init_fn = init_fn,
    };

    if (!vec_push(devfs_devices, &entry)) {
        log_warn("failed to grow devfs device registry");
        return false;
    }

    if (devfs_ready) {
        vfs_node_t *dev_dir = _ensure_dev_dir();
        if (!dev_dir || !init_fn(dev_dir)) {
            log_warn("%s registration failed", name);
            return false;
        }
    }

    return true;
}

bool devfs_unregister_device(const char *name) {
    if (!name || !name[0] || !devfs_devices) {
        return false;
    }

    for (size_t i = 0; i < devfs_devices->size; i++) {
        devfs_device_entry_t *entry = vec_at(devfs_devices, i);
        if (!entry) {
            continue;
        }

        const char *entry_name = entry->name;
        if (!entry_name || strcmp(entry_name, name)) {
            continue;
        }

        return vec_remove_at(devfs_devices, i, NULL);
    }

    return false;
}

bool devfs_unregister_node(const char *path) {
    if (!path || strncmp(path, "/dev/", 5) != 0) {
        return false;
    }

    vfs_node_t *node = vfs_lookup(path);
    if (!node) {
        return false;
    }

    if (sched_fd_refs_node(node)) {
        return false;
    }

    if (node->type == VFS_DIR) {
        return vfs_rmdir(path);
    }

    return vfs_unlink(path);
}

bool devfs_is_ready(void) {
    return devfs_ready;
}

static void _init_registered_devices(vfs_node_t *dev_dir) {
    if (!dev_dir || !devfs_devices) {
        return;
    }

    for (size_t i = 0; i < devfs_devices->size; i++) {
        devfs_device_entry_t *entry = vec_at(devfs_devices, i);
        if (!entry) {
            continue;
        }

        const char *name = entry->name ? entry->name : "device";
        devfs_device_init_fn init_fn = entry->init_fn;

        if (!init_fn || !init_fn(dev_dir)) {
            log_warn("%s registration failed", name);
        }
    }
}

static bool _register_builtin_nodes(vfs_node_t *dev_dir) {
    bool ok = true;

    vfs_interface_t *null_if =
        vfs_create_interface(_dev_null_read, _dev_null_write, NULL);

    if (
        !null_if ||
        !devfs_register_node(dev_dir, "null", VFS_CHARDEV, 0666, null_if, NULL)
    ) {
        log_warn("failed to create /dev/null");
        ok = false;
    }

    vfs_interface_t *zero_if =
        vfs_create_interface(_dev_zero_read, _dev_zero_write, NULL);

    if (
        !zero_if ||
        !devfs_register_node(dev_dir, "zero", VFS_CHARDEV, 0666, zero_if, NULL)
    ) {
        log_warn("failed to create /dev/zero");
        ok = false;
    }

    vfs_interface_t *os_if = vfs_create_interface(_dev_os_read, NULL, NULL);
    if (!os_if || !devfs_register_node(dev_dir, "os", VFS_CHARDEV, 0444, os_if, NULL)) {
        log_warn("failed to create /dev/os");
        ok = false;
    }

    vfs_interface_t *clock_if =
        vfs_create_interface(_dev_clock_read, NULL, NULL);

    if (
        !clock_if ||
        !devfs_register_node(dev_dir, "clock", VFS_CHARDEV, 0444, clock_if, NULL)
    ) {
        log_warn("failed to create /dev/clock");
        ok = false;
    }

    vfs_interface_t *swap_if = vfs_create_interface(_dev_swap_read, NULL, NULL);
    if (
        !swap_if ||
        !devfs_register_node(dev_dir, "swap", VFS_CHARDEV, 0444, swap_if, NULL)
    ) {
        log_warn("failed to create /dev/swap");
        ok = false;
    }

    vfs_interface_t *cpu_if = vfs_create_interface(_dev_cpu_read, NULL, NULL);
    if (!cpu_if || !devfs_register_node(dev_dir, "cpu", VFS_CHARDEV, 0444, cpu_if, NULL)) {
        log_warn("failed to create /dev/cpu");
        ok = false;
    }

    vfs_interface_t *stats_if = vfs_create_interface(_dev_stats_read, NULL, NULL);
    if (
        !stats_if ||
        !devfs_register_node(dev_dir, "stats", VFS_CHARDEV, 0444, stats_if, NULL)
    ) {
        log_warn("failed to create /dev/stats");
        ok = false;
    }

    return ok;
}

void devfs_init(void) {
    if (devfs_ready) {
        return;
    }

    _boot_seconds();

    vfs_node_t *dev_dir = _ensure_dev_dir();
    if (!dev_dir) {
        return;
    }

    _init_registered_devices(dev_dir);

    _register_builtin_nodes(dev_dir);
    devfs_ready = true;

    log_debug("devfs devices ready");
}
