#include <arch/arch.h>
#include <errno.h>
#include <log/log.h>
#include <poll.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <stdio.h>
#include <sys/devfs.h>
#include <sys/vfs.h>
#include <x86/serial.h>

#include "serial.h"

#define SERIAL_RX_QUEUE_CAP 1024

typedef struct {
    size_t index;
    size_t port;
    char rx_queue[SERIAL_RX_QUEUE_CAP];
    size_t rx_head;
    size_t rx_tail;
    size_t rx_count;
    spinlock_t rx_lock;
    sched_wait_queue_t rx_wait;
    bool rx_wait_ready;
} serial_port_t;

static serial_port_t serial_devices[] = {
    {.index = 0, .port = SERIAL_COM1},
    // we should probably add more
};


static bool serial_nodes_ready = false;
static bool serial_driver_loaded = false;

const driver_desc_t serial_driver_desc = {
    .name = "serial",
    .deps = NULL,
    .stage = DRIVER_STAGE_DEVFS,
    .load = serial_driver_load,
    .unload = serial_driver_unload,
    .is_busy = serial_driver_busy,
};

static bool _create_node(
    vfs_node_t *parent,
    const char *name,
    vfs_interface_t *interface,
    void *priv
) {
    vfs_node_t *node = vfs_lookup_from(parent, name);
    if (!node) {
        node = vfs_create(parent, (char *)name, VFS_CHARDEV, 0600);
    }

    if (!node) {
        return false;
    }

    node->type = VFS_CHARDEV;
    node->mode = 0600;
    node->interface = interface;
    node->private = priv;

    return true;
}

static ssize_t
_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)offset;

    if (!node || !node->private || !buf) {
        return -EINVAL;
    }

    if (!len) {
        return 0;
    }

    serial_port_t *serial = node->private;
    char *out = buf;

    for (;;) {
        size_t copied = 0;
        u32 wait_seq = sched_wait_seq(&serial->rx_wait);
        unsigned long irq_flags = spin_lock_irqsave(&serial->rx_lock);

        while (copied < len && serial->rx_count) {
            out[copied++] = serial->rx_queue[serial->rx_head];
            serial->rx_head = (serial->rx_head + 1) % SERIAL_RX_QUEUE_CAP;
            serial->rx_count--;
        }

        spin_unlock_irqrestore(&serial->rx_lock, irq_flags);

        if (copied) {
            return (ssize_t)copied;
        }

        if (flags & VFS_NONBLOCK) {
            return -EAGAIN;
        }

        if (!sched_is_running()) {
            continue;
        }

        sched_thread_t *current = sched_current();
        if (current && sched_signal_has_pending(current)) {
            return -EINTR;
        }

        (void)sched_block_if_unchanged(&serial->rx_wait, wait_seq);
    }
}

static ssize_t
_write(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)offset;
    (void)flags;

    if (!node || !node->private || !buf) {
        return -EINVAL;
    }

    serial_port_t *serial = node->private;

    if (!len) {
        return 0;
    }

    send_serial_sized_string(serial->port, buf, len);
    return (ssize_t)len;
}

static short _poll(vfs_node_t *node, short events, u32 flags) {
    (void)flags;

    if (!node || !node->private) {
        return POLLNVAL;
    }

    serial_port_t *serial = node->private;
    short ready = 0;

    unsigned long irq_flags = spin_lock_irqsave(&serial->rx_lock);

    if ((events & POLLIN) && serial->rx_count) {
        ready |= POLLIN;
    }

    if (events & POLLOUT) {
        ready |= POLLOUT;
    }

    spin_unlock_irqrestore(&serial->rx_lock, irq_flags);
    return ready;
}

static sched_wait_queue_t *
_wait_queue(vfs_node_t *node, short events, u32 flags) {
    (void)flags;

    if ((events & POLLIN) == 0 || (events & ~POLLIN) != 0) {
        return NULL;
    }

    if (!node || !node->private) {
        return NULL;
    }

    serial_port_t *serial = node->private;
    return &serial->rx_wait;
}

static bool _serial_register_devfs(vfs_node_t *dev_dir) {
    if (!dev_dir) {
        return false;
    }

    vfs_interface_t *serial_if = vfs_create_interface(_read, _write, NULL);
    if (!serial_if) {
        log_warn("interface alloc failed");
        return false;
    }

    serial_if->poll = _poll;
    serial_if->wait_queue = _wait_queue;

    char name[] = "ttySX";

    size_t count = sizeof(serial_devices) / sizeof(serial_devices[0]);

    for (size_t i = 0; i < count; i++) {
        if (!serial_devices[i].rx_wait_ready) {
            spinlock_init(&serial_devices[i].rx_lock);
            sched_wait_queue_init(&serial_devices[i].rx_wait);
            sched_wait_queue_set_name(&serial_devices[i].rx_wait, "serial_rx_wait");
            sched_wait_queue_set_poll_link(&serial_devices[i].rx_wait, true);
            serial_devices[i].rx_wait_ready = true;
        }

        name[4] = (char)('0' + serial_devices[i].index);

        if (!_create_node(dev_dir, name, serial_if, &serial_devices[i])) {
            log_warn("failed to create /dev/%s", name);
            continue;
        }

        log_debug("created /dev/%s", name);
    }

    serial_nodes_ready = true;
    return true;
}

void serial_push_rx(size_t index, char ch) {
    if (!ch || index >= sizeof(serial_devices) / sizeof(serial_devices[0])) {
        return;
    }

    serial_port_t *serial = &serial_devices[index];

    unsigned long irq_flags = spin_lock_irqsave(&serial->rx_lock);

    if (serial->rx_count == SERIAL_RX_QUEUE_CAP) {
        serial->rx_head = (serial->rx_head + 1) % SERIAL_RX_QUEUE_CAP;
        serial->rx_count--;
    }

    serial->rx_queue[serial->rx_tail] = ch;
    serial->rx_tail = (serial->rx_tail + 1) % SERIAL_RX_QUEUE_CAP;
    serial->rx_count++;

    spin_unlock_irqrestore(&serial->rx_lock, irq_flags);

    if (sched_is_running()) {
        sched_wake_one(&serial->rx_wait);
    }
}

bool serial_driver_busy(void) {
    char path[32] = {0};
    size_t count = sizeof(serial_devices) / sizeof(serial_devices[0]);

    for (size_t i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "/dev/ttyS%zu", serial_devices[i].index);
        vfs_node_t *node = vfs_lookup(path);
        if (node && sched_fd_refs_node(node)) {
            return true;
        }
    }

    return false;
}

driver_err_t serial_driver_load(void) {
    if (serial_driver_loaded) {
        return DRIVER_OK;
    }

    if (!devfs_register_device("serial", _serial_register_devfs)) {
        return DRIVER_ERR_INIT_FAILED;
    }

    serial_driver_loaded = true;
    return DRIVER_OK;
}

driver_err_t serial_driver_unload(void) {
    if (!serial_driver_loaded) {
        return DRIVER_OK;
    }

    if (serial_driver_busy()) {
        return DRIVER_ERR_BUSY;
    }

    size_t count = sizeof(serial_devices) / sizeof(serial_devices[0]);
    char path[32] = {0};

    for (size_t i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "/dev/ttyS%zu", serial_devices[i].index);
        vfs_node_t *node = vfs_lookup(path);
        if (node && !devfs_unregister_node(path)) {
            return DRIVER_ERR_BUSY;
        }
    }

    if (!devfs_unregister_device("serial")) {
        log_warn("failed to unregister serial devfs callback");
    }

    serial_nodes_ready = false;
    serial_driver_loaded = false;
    return DRIVER_OK;
}
