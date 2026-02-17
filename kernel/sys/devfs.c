#include "devfs.h"

#include <arch/arch.h>
#include <base/units.h>
#include <errno.h>
#include <inttypes.h>
#include <log/log.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cpu.h>
#include <sys/stat.h>

#include "vfs.h"

#define SYSINFO_TEXT_MAX 384
#define DEVFS_MAX_DEVICES 32

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
    const char* name;
    devfs_device_init_fn init_fn;
} devfs_device_entry_t;

static devfs_device_entry_t devfs_devices[DEVFS_MAX_DEVICES];
static size_t devfs_device_count = 0;

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

static ssize_t _dev_os_read(vfs_node_t* node, void* buf, size_t offset, size_t len, u32 flags) {
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

bool devfs_register_node(
    vfs_node_t* parent,
    const char* name,
    u32 type,
    mode_t mode,
    vfs_interface_t* interface,
    void* priv
) {
    if (!parent || !name)
        return false;

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

vfs_node_t* devfs_register_dir(vfs_node_t* parent, const char* name, mode_t mode) {
    if (!parent || !name)
        return NULL;

    vfs_node_t* node = vfs_lookup_from(parent, name);
    if (!node)
        node = vfs_create(parent, (char*)name, VFS_DIR, mode);

    if (!node)
        return NULL;

    node->type = VFS_DIR;
    node->mode = mode;
    node->interface = NULL;
    node->private = NULL;
    return node;
}

bool devfs_register_device(const char* name, devfs_device_init_fn init_fn) {
    if (!name || !init_fn)
        return false;

    for (size_t i = 0; i < devfs_device_count; i++) {
        if (devfs_devices[i].init_fn == init_fn)
            return true;
    }

    if (devfs_device_count >= DEVFS_MAX_DEVICES) {
        log_warn("devfs: device registry full");
        return false;
    }

    devfs_devices[devfs_device_count].name = name;
    devfs_devices[devfs_device_count].init_fn = init_fn;
    devfs_device_count++;
    return true;
}

static void _init_registered_devices(vfs_node_t* dev_dir) {
    for (size_t i = 0; i < devfs_device_count; i++) {
        const char* name = devfs_devices[i].name ? devfs_devices[i].name : "device";
        devfs_device_init_fn init_fn = devfs_devices[i].init_fn;

        if (!init_fn || !init_fn(dev_dir))
            log_warn("devfs: %s registration failed", name);
    }
}

static vfs_node_t* _ensure_dev_dir(void) {
    vfs_node_t* root = vfs_lookup("/");
    if (!root) {
        log_warn("devfs: missing root");
        return NULL;
    }

    vfs_node_t* dev_dir = vfs_lookup("/dev");
    if (!dev_dir)
        dev_dir = vfs_create(root, "dev", VFS_DIR, 0755);

    if (!dev_dir) {
        log_warn("devfs: failed to create /dev");
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

static bool _register_builtin_nodes(vfs_node_t* dev_dir) {
    bool ok = true;

    vfs_interface_t* null_if = vfs_create_interface(_dev_null_read, _dev_null_write, NULL);
    if (!null_if || !devfs_register_node(dev_dir, "null", VFS_CHARDEV, 0666, null_if, NULL)) {
        log_warn("devfs: failed to create /dev/null");
        ok = false;
    }

    vfs_interface_t* zero_if = vfs_create_interface(_dev_zero_read, _dev_zero_write, NULL);
    if (!zero_if || !devfs_register_node(dev_dir, "zero", VFS_CHARDEV, 0666, zero_if, NULL)) {
        log_warn("devfs: failed to create /dev/zero");
        ok = false;
    }

    vfs_interface_t* os_if = vfs_create_interface(_dev_os_read, NULL, NULL);
    if (!os_if || !devfs_register_node(dev_dir, "os", VFS_CHARDEV, 0444, os_if, NULL)) {
        log_warn("devfs: failed to create /dev/os");
        ok = false;
    }

    vfs_interface_t* clock_if = vfs_create_interface(_dev_clock_read, NULL, NULL);
    if (!clock_if || !devfs_register_node(dev_dir, "clock", VFS_CHARDEV, 0444, clock_if, NULL)) {
        log_warn("devfs: failed to create /dev/clock");
        ok = false;
    }

    vfs_interface_t* swap_if = vfs_create_interface(_dev_swap_read, NULL, NULL);
    if (!swap_if || !devfs_register_node(dev_dir, "swap", VFS_CHARDEV, 0444, swap_if, NULL)) {
        log_warn("devfs: failed to create /dev/swap");
        ok = false;
    }

    vfs_interface_t* cpu_if = vfs_create_interface(_dev_cpu_read, NULL, NULL);
    if (!cpu_if || !devfs_register_node(dev_dir, "cpu", VFS_CHARDEV, 0444, cpu_if, NULL)) {
        log_warn("devfs: failed to create /dev/cpu");
        ok = false;
    }

    return ok;
}

void devfs_init(void) {
    _boot_seconds();

    vfs_node_t* dev_dir = _ensure_dev_dir();
    if (!dev_dir)
        return;

    _init_registered_devices(dev_dir);

    _register_builtin_nodes(dev_dir);

    log_info("devfs: devices ready");
}
