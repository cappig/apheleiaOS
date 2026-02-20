#include <arch/arch.h>
#include <errno.h>
#include <log/log.h>
#include <poll.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <sys/vfs.h>

#include "serial.h"

#define SERIAL_RX_QUEUE_CAP 1024

typedef struct {
    size_t index;
    size_t port;
    char rx_queue[SERIAL_RX_QUEUE_CAP];
    size_t rx_head;
    size_t rx_tail;
    size_t rx_count;
    sched_wait_queue_t rx_wait;
    bool rx_wait_ready;
} serial_dev_t;

static serial_dev_t serial_devices[] = {
    {.index = 0, .port = SERIAL_COM1},
    // we should probably add more
};


static bool serial_nodes_ready = false;

static bool _create_node(
    vfs_node_t *parent,
    const char *name,
    vfs_interface_t *interface,
    void *priv
) {
    vfs_node_t *node = vfs_lookup_from(parent, name);
    if (!node) {
        node = vfs_create(parent, (char *)name, VFS_CHARDEV, 0666);
    }

    if (!node) {
        return false;
    }

    node->type = VFS_CHARDEV;
    node->mode = 0666;
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

    serial_dev_t *serial = node->private;
    char *out = buf;

    for (;;) {
        size_t copied = 0;
        unsigned long irq_flags = arch_irq_save();

        while (copied < len && serial->rx_count) {
            out[copied++] = serial->rx_queue[serial->rx_head];
            serial->rx_head = (serial->rx_head + 1) % SERIAL_RX_QUEUE_CAP;
            serial->rx_count--;
        }

        arch_irq_restore(irq_flags);

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

        sched_block(&serial->rx_wait);
    }
}

static ssize_t
_write(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)offset;
    (void)flags;

    if (!node || !node->private || !buf) {
        return -EINVAL;
    }

    serial_dev_t *serial = node->private;

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

    serial_dev_t *serial = node->private;
    short ready = 0;

    unsigned long irq_flags = arch_irq_save();

    if ((events & POLLIN) && serial->rx_count) {
        ready |= POLLIN;
    }

    if (events & POLLOUT) {
        ready |= POLLOUT;
    }

    arch_irq_restore(irq_flags);
    return ready;
}

void serial_devfs_init(void) {
    if (serial_nodes_ready) {
        return;
    }

    vfs_node_t *dev_dir = vfs_lookup("/dev");
    if (!dev_dir) {
        log_warn("/dev missing");
        return;
    }

    vfs_interface_t *serial_if = vfs_create_interface(_read, _write, NULL);
    if (!serial_if) {
        log_warn("interface alloc failed");
        return;
    }

    serial_if->poll = _poll;

    char name[] = "ttySX";

    size_t count = sizeof(serial_devices) / sizeof(serial_devices[0]);

    for (size_t i = 0; i < count; i++) {
        if (!serial_devices[i].rx_wait_ready) {
            sched_wait_queue_init(&serial_devices[i].rx_wait);
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
}

void serial_dev_push_rx(size_t index, char ch) {
    if (!ch || index >= sizeof(serial_devices) / sizeof(serial_devices[0])) {
        return;
    }

    serial_dev_t *serial = &serial_devices[index];

    unsigned long irq_flags = arch_irq_save();

    if (serial->rx_count == SERIAL_RX_QUEUE_CAP) {
        serial->rx_head = (serial->rx_head + 1) % SERIAL_RX_QUEUE_CAP;
        serial->rx_count--;
    }

    serial->rx_queue[serial->rx_tail] = ch;
    serial->rx_tail = (serial->rx_tail + 1) % SERIAL_RX_QUEUE_CAP;
    serial->rx_count++;

    arch_irq_restore(irq_flags);

    if (sched_is_running()) {
        sched_wake_one(&serial->rx_wait);
    }
}
