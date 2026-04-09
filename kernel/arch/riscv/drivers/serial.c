#include <errno.h>
#include <poll.h>
#include <riscv/console.h>
#include <riscv/serial.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <sys/devfs.h>
#include <sys/lock.h>
#include <sys/vfs.h>

#include "serial.h"

#define RX_CAP 1024

typedef struct {
    char rx_buf[RX_CAP];
    size_t rx_head;
    size_t rx_tail;
    size_t rx_count;

    spinlock_t rx_lock;
    spinlock_t tx_lock;

    sched_wait_queue_t rx_wait;

    bool ready;
    bool registered;
} serial_port_t;

static serial_port_t port = {
    .rx_lock = SPINLOCK_INIT,
    .tx_lock = SPINLOCK_INIT,
};

static bool driver_loaded = false;

const driver_desc_t serial_driver_desc = {
    .name = "riscv-serial",
    .deps = NULL,
    .stage = DRIVER_STAGE_DEVFS,
    .load = serial_driver_load,
    .unload = serial_driver_unload,
    .is_busy = serial_driver_busy,
};

static ssize_t
_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)offset;

    if (!node || !buf) {
        return -EINVAL;
    }
    if (!len) {
        return 0;
    }

    char *out = buf;

    for (;;) {
        size_t copied = 0;
        u32 wait_seq = sched_wait_seq(&port.rx_wait);
        unsigned long irq_flags = spin_lock_irqsave(&port.rx_lock);

        while (copied < len && port.rx_count) {
            out[copied++] = port.rx_buf[port.rx_head];
            port.rx_head = (port.rx_head + 1) % RX_CAP;
            port.rx_count--;
        }

        spin_unlock_irqrestore(&port.rx_lock, irq_flags);

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

        (void)sched_block_if_unchanged(&port.rx_wait, wait_seq);
    }
}

static ssize_t
_write(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)node;
    (void)offset;
    (void)flags;

    if (!buf) {
        return -EINVAL;
    }
    if (!len) {
        return 0;
    }

    unsigned long irq_flags = spin_lock_irqsave(&port.tx_lock);
    send_serial_sized_string(uart_console_base(), buf, len);
    spin_unlock_irqrestore(&port.tx_lock, irq_flags);
    return (ssize_t)len;
}

static short _poll(vfs_node_t *node, short events, u32 flags) {
    (void)flags;

    if (!node) {
        return POLLNVAL;
    }

    short ready = 0;
    unsigned long irq_flags = spin_lock_irqsave(&port.rx_lock);

    if ((events & POLLIN) && port.rx_count) {
        ready |= POLLIN;
    }
    if (events & POLLOUT) {
        ready |= POLLOUT;
    }

    spin_unlock_irqrestore(&port.rx_lock, irq_flags);
    return ready;
}

static sched_wait_queue_t *_wait_queue(vfs_node_t *node, short events, u32 flags) {
    (void)node;
    (void)flags;

    if (events != POLLIN) {
        return NULL;
    }

    return &port.rx_wait;
}

static bool _register_devfs(vfs_node_t *dev_dir) {
    if (!dev_dir) {
        return false;
    }

    vfs_interface_t *iface = vfs_create_interface(_read, _write, NULL);
    if (!iface) {
        return false;
    }

    iface->poll = _poll;
    iface->wait_queue = _wait_queue;

    vfs_node_t *node = vfs_lookup_from(dev_dir, "ttyS0");
    if (!node) {
        node = vfs_create(dev_dir, "ttyS0", VFS_CHARDEV, 0600);
    }

    if (!node) {
        return false;
    }

    node->type = VFS_CHARDEV;
    node->mode = 0600;
    node->interface = iface;
    node->private = &port;

    port.registered = true;
    return true;
}

driver_err_t serial_driver_load(void) {
    if (driver_loaded) {
        return DRIVER_ERR_ALREADY_LOADED;
    }

    if (!port.ready) {
        sched_wait_queue_init(&port.rx_wait);
        sched_wait_queue_set_poll_link(&port.rx_wait, true);
        port.ready = true;
    }

    if (!devfs_register_device("riscv-serial", _register_devfs)) {
        return DRIVER_ERR_INIT_FAILED;
    }

    driver_loaded = true;
    return DRIVER_OK;
}

driver_err_t serial_driver_unload(void) {
    if (!driver_loaded) {
        return DRIVER_ERR_NOT_LOADED;
    }

    if (port.registered) {
        vfs_node_t *node = vfs_lookup("/dev/ttyS0");
        if (node && !devfs_unregister_node("/dev/ttyS0")) {
            return DRIVER_ERR_BUSY;
        }
    }

    if (!devfs_unregister_device("riscv-serial")) {
        return DRIVER_ERR_BUSY;
    }

    port.registered = false;
    driver_loaded = false;
    return DRIVER_OK;
}

bool serial_driver_busy(void) {
    vfs_node_t *node = vfs_lookup("/dev/ttyS0");
    return node && sched_fd_refs_node(node);
}

void serial_rx_push(char ch) {
    if (!port.ready) {
        return;
    }

    unsigned long irq_flags = spin_lock_irqsave(&port.rx_lock);

    if (port.rx_count == RX_CAP) {
        port.rx_head = (port.rx_head + 1) % RX_CAP;
        port.rx_count--;
    }

    port.rx_buf[port.rx_tail] = ch;
    port.rx_tail = (port.rx_tail + 1) % RX_CAP;
    port.rx_count++;

    spin_unlock_irqrestore(&port.rx_lock, irq_flags);
    sched_wake_all(&port.rx_wait);
}
