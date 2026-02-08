#include "devfs.h"

#include <log/log.h>
#include <stddef.h>
#include <sys/stat.h>

#include "tty.h"
#include "vfs.h"

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

static bool _create_node(
    vfs_node_t* parent,
    const char* name,
    u32 type,
    mode_t mode,
    vfs_interface_t* interface,
    void* priv
) {
    vfs_node_t* node = vfs_create(parent, (char*)name, type, mode);

    if (!node)
        return false;

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
    char name[] = "ttyC0";

    for (size_t i = 0; i < TTY_COUNT; i++) {
        name[4] = (char)('0' + i);
        _create_node(dev_dir, name, VFS_CHARDEV, 0666, tty_if, &tty_handles[i]);
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

    vfs_interface_t* tty_if = vfs_create_interface(_dev_tty_read, _dev_tty_write);

    if (!tty_if) {
        log_warn("devfs: failed to allocate tty interface");
        return;
    }

    if (!_create_node(dev_dir, "tty", VFS_CHARDEV, 0666, tty_if, &tty_current))
        log_warn("devfs: failed to create /dev/tty");

    if (!_create_node(dev_dir, "console", VFS_CHARDEV, 0666, tty_if, &tty_console))
        log_warn("devfs: failed to create /dev/console");

    _create_ttys(dev_dir, tty_if);

    log_info("devfs: ttys ready");
}
