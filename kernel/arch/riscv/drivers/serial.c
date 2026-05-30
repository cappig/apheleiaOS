#include "serial.h"

#include <errno.h>
#include <poll.h>
#include <sched/scheduler.h>
#include <sys/devfs.h>
#include <sys/tty.h>
#include <sys/vfs.h>

static tty_handle_t serial_console = {
    .kind = TTY_HANDLE_NAMED,
    .index = 0,
};

// the devfs node is a view of the boot console, not a second UART owner
typedef struct {
    bool loaded;
    bool node_ready;
} serial_driver_state_t;

static serial_driver_state_t serial_driver = { 0 };

const driver_desc_t serial_driver_desc = {
    .name = "riscv-serial",
    .deps = NULL,
    .stage = DRIVER_STAGE_DEVFS,
    .load = serial_driver_load,
    .unload = serial_driver_unload,
    .is_busy = serial_driver_busy,
};

static ssize_t serial_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)offset;
    (void)flags;

    return tty_read_handle(node ? node->private : NULL, buf, len);
}

static ssize_t serial_write(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)offset;
    (void)flags;

    return tty_write_handle(node ? node->private : NULL, buf, len);
}

static ssize_t serial_ioctl(vfs_node_t *node, u64 request, void *args) {
    return tty_ioctl_handle(node ? node->private : NULL, request, args);
}

static short serial_poll(vfs_node_t *node, short events, u32 flags) {
    return tty_poll_handle(node ? node->private : NULL, events, flags);
}

static sched_wait_queue_t *serial_wait_queue(vfs_node_t *node, short events, u32 flags) {
    return tty_wait_queue_handle(node ? node->private : NULL, events, flags);
}

static bool register_serial_dev(vfs_node_t *dev_dir) {
    if (!dev_dir) {
        return false;
    }

    vfs_interface_t *iface = vfs_create_interface(serial_read, serial_write, NULL);
    if (!iface) {
        return false;
    }

    iface->ioctl = serial_ioctl;
    iface->poll = serial_poll;
    iface->wait_queue = serial_wait_queue;

    // keep /dev/ttyS0 root only so arbitrary users cannot scribble on firmware logs
    serial_driver.node_ready = devfs_register_node(dev_dir, "ttyS0", VFS_CHARDEV, 0600, iface, &serial_console);
    return serial_driver.node_ready;
}

driver_err_t serial_driver_load(void) {
    if (serial_driver.loaded) {
        return DRIVER_OK;
    }

    if (!devfs_register_device("riscv-serial", register_serial_dev)) {
        return DRIVER_ERR_INIT_FAILED;
    }

    serial_driver.loaded = true;
    return DRIVER_OK;
}

driver_err_t serial_driver_unload(void) {
    if (!serial_driver.loaded) {
        return DRIVER_OK;
    }

    if (serial_driver_busy()) {
        return DRIVER_ERR_BUSY;
    }

    if (serial_driver.node_ready && !devfs_unregister_node("/dev/ttyS0")) {
        return DRIVER_ERR_BUSY;
    }

    if (!devfs_unregister_device("riscv-serial")) {
        return DRIVER_ERR_BUSY;
    }

    serial_driver.node_ready = false;
    serial_driver.loaded = false;
    return DRIVER_OK;
}

bool serial_driver_busy(void) {
    vfs_node_t *node = vfs_lookup("/dev/ttyS0");
    return node && sched_fd_refs_node(node);
}
