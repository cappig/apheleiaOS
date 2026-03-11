#include "pty.h"

#include <arch/arch.h>
#include <data/ring.h>
#include <errno.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <signal.h>
#include <string.h>
#include <sys/devfs.h>
#include <sys/ioctl.h>

#include "vfs.h"

typedef struct {
    u8 data[PTY_BUFFER_SIZE];
    ring_io_t ring;
    spinlock_t lock;
    sched_wait_queue_t read_wait;
    sched_wait_queue_t write_wait;
    bool ready;
} pty_queue_t;

typedef struct {
    pty_queue_t master_rx;
    pty_queue_t slave_rx;
    termios_t termios;
    winsize_t winsize;
    pid_t pgrp;
    bool allocated;
    size_t refs;
} pty_t;

static pty_t ptys[PTY_COUNT] = {0};
static pty_handle_t pty_master_handles[PTY_COUNT];
static pty_handle_t pty_slave_handles[PTY_COUNT];
static pty_handle_t pty_master_default = {.index = 0, .is_master = true};
static spinlock_t pty_state_lock = SPINLOCK_INIT;


static void _queue_reset(pty_queue_t *queue) {
    if (!queue) {
        return;
    }

    ring_io_init(&queue->ring, queue->data, PTY_BUFFER_SIZE);
}

static void _reset_state(pty_t *pty) {
    if (!pty) {
        return;
    }

    _queue_reset(&pty->master_rx);
    _queue_reset(&pty->slave_rx);
    __termios_default_init(&pty->termios);

    pty->winsize.ws_col = 80;
    pty->winsize.ws_row = 25;
    pty->winsize.ws_xpixel = 0;
    pty->winsize.ws_ypixel = 0;
    pty->pgrp = 0;
    pty->refs = 0;
}

static bool _handle_valid(const pty_handle_t *handle) {
    if (!handle) {
        return false;
    }

    return handle->index < PTY_COUNT;
}

static pty_t *_handle_pty(const pty_handle_t *handle) {
    if (!_handle_valid(handle)) {
        return NULL;
    }

    return &ptys[handle->index];
}

static pty_queue_t *_handle_rx_queue(pty_t *pty, const pty_handle_t *handle) {
    if (!pty || !handle) {
        return NULL;
    }

    if (handle->is_master) {
        return &pty->master_rx;
    }

    return &pty->slave_rx;
}

static pty_queue_t *_handle_tx_queue(pty_t *pty, const pty_handle_t *handle) {
    if (!pty || !handle) {
        return NULL;
    }

    if (handle->is_master) {
        return &pty->slave_rx;
    }

    return &pty->master_rx;
}

static void _queue_init(pty_queue_t *queue) {
    if (!queue || queue->ready) {
        return;
    }

    _queue_reset(queue);
    spinlock_init(&queue->lock);

    sched_wait_queue_init(&queue->read_wait);
    sched_wait_queue_init(&queue->write_wait);
    sched_wait_queue_set_name(&queue->read_wait, "pty_read_wait");
    sched_wait_queue_set_name(&queue->write_wait, "pty_write_wait");
    sched_wait_queue_set_poll_link(&queue->read_wait, true);
    sched_wait_queue_set_poll_link(&queue->write_wait, true);

    queue->ready = true;
}

static size_t _queue_size(pty_queue_t *queue) {
    if (!queue) {
        return 0;
    }

    unsigned long irq_flags = spin_lock_irqsave(&queue->lock);
    size_t used = ring_io_size(&queue->ring);
    spin_unlock_irqrestore(&queue->lock, irq_flags);

    return used;
}

static size_t _queue_free_space(pty_queue_t *queue) {
    if (!queue) {
        return 0;
    }

    unsigned long irq_flags = spin_lock_irqsave(&queue->lock);
    size_t free_space = ring_io_free_space(&queue->ring);
    spin_unlock_irqrestore(&queue->lock, irq_flags);

    return free_space;
}

static void _seed_handles(void) {
    for (size_t i = 0; i < PTY_COUNT; i++) {
        pty_master_handles[i].index = i;
        pty_master_handles[i].is_master = true;
        pty_slave_handles[i].index = i;
        pty_slave_handles[i].is_master = false;
    }
}

static ssize_t _dev_pty_read(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)offset;
    return pty_read_handle(node ? node->private : NULL, buf, len, flags);
}

static ssize_t _dev_pty_write(
    vfs_node_t *node,
    void *buf,
    size_t offset,
    size_t len,
    u32 flags
) {
    (void)offset;
    return pty_write_handle(node ? node->private : NULL, buf, len, flags);
}

static ssize_t _dev_pty_ioctl(vfs_node_t *node, u64 request, void *args) {
    return pty_ioctl_handle(node ? node->private : NULL, request, args);
}

static short _dev_pty_poll(vfs_node_t *node, short events, u32 flags) {
    return pty_poll_handle(node ? node->private : NULL, events, flags);
}

static sched_wait_queue_t *
_pty_wait_queue_handle(const pty_handle_t *handle, short events, u32 flags) {
    (void)flags;

    pty_t *pty = _handle_pty(handle);
    if (!pty) {
        return NULL;
    }

    pty_queue_t *rx = _handle_rx_queue(pty, handle);
    pty_queue_t *tx = _handle_tx_queue(pty, handle);

    if ((events & POLLIN) && (events & ~POLLIN) == 0) {
        return rx ? &rx->read_wait : NULL;
    }

    if ((events & POLLOUT) && (events & ~POLLOUT) == 0) {
        return tx ? &tx->write_wait : NULL;
    }

    return NULL;
}

static sched_wait_queue_t *
_dev_pty_wait_queue(vfs_node_t *node, short events, u32 flags) {
    return _pty_wait_queue_handle(node ? node->private : NULL, events, flags);
}

static void _queue_clear(pty_queue_t *queue) {
    if (!queue) {
        return;
    }

    unsigned long irq_flags = spin_lock_irqsave(&queue->lock);
    ring_io_reset(&queue->ring);
    spin_unlock_irqrestore(&queue->lock, irq_flags);
}

static u64 _vtime_to_ticks(cc_t vtime) {
    if (!vtime) {
        return 0;
    }

    u32 hz = arch_timer_hz();
    if (!hz) {
        return 1;
    }

    u64 ticks = ((u64)vtime * (u64)hz + 9ULL) / 10ULL;
    if (!ticks) {
        ticks = 1;
    }

    return ticks;
}

static ssize_t
_queue_read(pty_queue_t *queue, void *buf, size_t len, bool nonblock) {
    if (!queue || !buf) {
        return -EINVAL;
    }

    if (!len) {
        return 0;
    }

    size_t total = 0;

    for (;;) {
        u32 wait_seq = sched_wait_seq(&queue->read_wait);
        unsigned long irq_flags = spin_lock_irqsave(&queue->lock);

        size_t read_now = ring_io_read(&queue->ring, (u8 *)buf + total, len - total);

        spin_unlock_irqrestore(&queue->lock, irq_flags);

        if (read_now) {
            total += read_now;
            sched_wake_one(&queue->write_wait);

            if (total == len) {
                return (ssize_t)total;
            }
        }

        if (total) {
            return (ssize_t)total;
        }

        if (nonblock) {
            return -EAGAIN;
        }

        if (!sched_is_running()) {
            continue;
        }

        sched_thread_t *current = sched_current();

        if (current && sched_signal_has_pending(current)) {
            return -EINTR;
        }

        sched_wait_result_t wait_result = sched_wait_on_queue(
            &queue->read_wait,
            wait_seq,
            0,
            SCHED_WAIT_INTERRUPTIBLE
        );
        if (wait_result == SCHED_WAIT_INTR) {
            return -EINTR;
        }
    }
}

static ssize_t _queue_read_termios(
    pty_queue_t *queue,
    void *buf,
    size_t len,
    bool nonblock,
    const termios_t *tos
) {
    if (!queue || !buf || !tos) {
        return -EINVAL;
    }

    if (!len) {
        return 0;
    }

    if (tos->c_lflag & ICANON) {
        return _queue_read(queue, buf, len, nonblock);
    }

    size_t vmin = (size_t)tos->c_cc[VMIN];
    size_t target = vmin ? (vmin < len ? vmin : len) : 1;
    u64 timeout_ticks = _vtime_to_ticks(tos->c_cc[VTIME]);

    size_t total = 0;
    bool first_byte_seen = false;
    u64 deadline = 0;

    for (;;) {
        u32 wait_seq = sched_wait_seq(&queue->read_wait);
        unsigned long irq_flags = spin_lock_irqsave(&queue->lock);

        size_t read_now = ring_io_read(&queue->ring, (u8 *)buf + total, len - total);

        spin_unlock_irqrestore(&queue->lock, irq_flags);

        if (read_now) {
            total += read_now;
            first_byte_seen = true;
            sched_wake_one(&queue->write_wait);

            if (total == len) {
                return (ssize_t)total;
            }

            if (!vmin) {
                return (ssize_t)total;
            }

            if (total >= target) {
                return (ssize_t)total;
            }

            if (timeout_ticks) {
                deadline = arch_timer_ticks() + timeout_ticks;
            }

            continue;
        }

        if (total) {
            if (!vmin) {
                return (ssize_t)total;
            }

            if (timeout_ticks && arch_timer_ticks() >= deadline) {
                return (ssize_t)total;
            }
        } else {
            if (!vmin && !timeout_ticks) {
                return 0;
            }

            if (!vmin && timeout_ticks) {
                if (!deadline) {
                    deadline = arch_timer_ticks() + timeout_ticks;
                }

                if (arch_timer_ticks() >= deadline) {
                    return 0;
                }
            }
        }

        if (nonblock) {
            return total ? (ssize_t)total : -EAGAIN;
        }

        if (!sched_is_running()) {
            arch_cpu_wait();
            continue;
        }

        sched_thread_t *current = sched_current();
        if (current && sched_signal_has_pending(current)) {
            return total ? (ssize_t)total : -EINTR;
        }

        bool use_timeout = false;
        if (!vmin && timeout_ticks) {
            use_timeout = true;
        } else if (vmin && timeout_ticks && first_byte_seen) {
            use_timeout = true;
        }

        if (use_timeout) {
            sched_wait_result_t wait_result = sched_wait_on_queue(
                &queue->read_wait,
                wait_seq,
                deadline,
                SCHED_WAIT_INTERRUPTIBLE
            );
            if (wait_result == SCHED_WAIT_TIMEOUT) {
                return (ssize_t)total;
            }
            if (wait_result == SCHED_WAIT_INTR) {
                return total ? (ssize_t)total : -EINTR;
            }
            continue;
        }

        sched_wait_result_t wait_result = sched_wait_on_queue(
            &queue->read_wait,
            wait_seq,
            0,
            SCHED_WAIT_INTERRUPTIBLE
        );
        if (wait_result == SCHED_WAIT_INTR) {
            return total ? (ssize_t)total : -EINTR;
        }
    }
}

static ssize_t
_queue_write(pty_queue_t *queue, const void *buf, size_t len, bool nonblock) {
    if (!queue || !buf) {
        return -EINVAL;
    }

    if (!len) {
        return 0;
    }

    size_t total = 0;

    for (;;) {
        u32 wait_seq = sched_wait_seq(&queue->write_wait);
        unsigned long irq_flags = spin_lock_irqsave(&queue->lock);

        size_t wrote_now = ring_io_write(
            &queue->ring,
            (const u8 *)buf + total,
            len - total
        );

        spin_unlock_irqrestore(&queue->lock, irq_flags);

        if (wrote_now) {
            total += wrote_now;
            sched_wake_one(&queue->read_wait);

            if (total == len) {
                return (ssize_t)total;
            }
        }

        if (total == len) {
            return (ssize_t)total;
        }

        if (nonblock) {
            return total ? (ssize_t)total : -EAGAIN;
        }

        if (!sched_is_running()) {
            continue;
        }

        sched_thread_t *current = sched_current();
        if (current && sched_signal_has_pending(current)) {
            return total ? (ssize_t)total : -EINTR;
        }

        sched_wait_result_t wait_result = sched_wait_on_queue(
            &queue->write_wait,
            wait_seq,
            0,
            SCHED_WAIT_INTERRUPTIBLE
        );
        if (wait_result == SCHED_WAIT_INTR) {
            return total ? (ssize_t)total : -EINTR;
        }
    }
}

static bool pty_register_devfs(vfs_node_t *dev_dir) {
    if (!dev_dir) {
        return false;
    }

    if (PTY_COUNT && !ptys[0].master_rx.ready) {
        log_warn("PTY state not initialized");
        return false;
    }

    _seed_handles();

    vfs_interface_t *pty_if =
        vfs_create_interface(_dev_pty_read, _dev_pty_write, NULL);

    if (!pty_if) {
        log_warn("PTY failed to allocate /dev interface");
        return false;
    }

    pty_if->ioctl = _dev_pty_ioctl;
    pty_if->poll = _dev_pty_poll;
    pty_if->wait_queue = _dev_pty_wait_queue;

    bool ok = true;

    bool ptmx_registered = devfs_register_node(
        dev_dir,
        "ptmx",
        VFS_CHARDEV,
        0666,
        pty_if,
        &pty_master_default
    );

    if (!ptmx_registered) {
        log_warn("failed to create /dev/ptmx");
        ok = false;
    }

    char pty_name[] = "pty0";
    char pts_name[] = "pts0";

    for (size_t i = 0; i < PTY_COUNT; i++) {
        pty_name[3] = (char)('0' + i);
        pts_name[3] = (char)('0' + i);
        pty_handle_t *master_handle = &pty_master_handles[i];
        pty_handle_t *slave_handle = &pty_slave_handles[i];

        bool master_registered = devfs_register_node(
            dev_dir, pty_name, VFS_CHARDEV, 0666, pty_if, master_handle
        );

        if (!master_registered) {
            log_warn("failed to create /dev/%s", pty_name);
            ok = false;
        }

        bool slave_registered = devfs_register_node(
            dev_dir, pts_name, VFS_CHARDEV, 0666, pty_if, slave_handle
        );
        
        if (!slave_registered) {
            log_warn("failed to create /dev/%s", pts_name);
            ok = false;
        }
    }

    return ok;
}

void pty_init(void) {
    if (!devfs_register_device("pty", pty_register_devfs)) {
        log_warn("failed to register devfs init callback");
    }

    if (PTY_COUNT && ptys[0].master_rx.ready) {
        return;
    }

    for (size_t i = 0; i < PTY_COUNT; i++) {
        pty_t *pty = &ptys[i];

        _queue_init(&pty->master_rx);
        _queue_init(&pty->slave_rx);
        _reset_state(pty);
        pty->allocated = false;
    }
}

bool pty_reserve(size_t *index_out) {
    if (!index_out) {
        return false;
    }

    unsigned long irq_flags = spin_lock_irqsave(&pty_state_lock);

    for (size_t i = 0; i < PTY_COUNT; i++) {
        pty_t *pty = &ptys[i];

        if (pty->allocated) {
            continue;
        }

        _reset_state(pty);
        pty->allocated = true;
        *index_out = i;

        spin_unlock_irqrestore(&pty_state_lock, irq_flags);
        return true;
    }

    spin_unlock_irqrestore(&pty_state_lock, irq_flags);
    return false;
}

void pty_unreserve(size_t index) {
    if (index >= PTY_COUNT) {
        return;
    }

    unsigned long irq_flags = spin_lock_irqsave(&pty_state_lock);
    pty_t *pty = &ptys[index];

    if (pty->allocated && !pty->refs) {
        _reset_state(pty);
        pty->allocated = false;
    }

    spin_unlock_irqrestore(&pty_state_lock, irq_flags);
}

void pty_hold(size_t index) {
    if (index >= PTY_COUNT) {
        return;
    }

    unsigned long irq_flags = spin_lock_irqsave(&pty_state_lock);

    pty_t *pty = &ptys[index];

    if (pty->allocated) {
        pty->refs++;
    }

    spin_unlock_irqrestore(&pty_state_lock, irq_flags);
}

void pty_put(size_t index) {
    if (index >= PTY_COUNT) {
        return;
    }

    pid_t hup_pgrp = 0;
    unsigned long irq_flags = spin_lock_irqsave(&pty_state_lock);
    pty_t *pty = &ptys[index];

    if (pty->allocated && pty->refs) {
        pty->refs--;

        if (!pty->refs) {
            hup_pgrp = pty->pgrp;
            _reset_state(pty);
            pty->allocated = false;
        }
    }

    spin_unlock_irqrestore(&pty_state_lock, irq_flags);

    if (hup_pgrp > 0) {
        (void)sched_signal_send_pgrp(hup_pgrp, SIGHUP);
        (void)sched_signal_send_pgrp(hup_pgrp, SIGCONT);
    }
}

ssize_t
pty_read_handle(const pty_handle_t *handle, void *buf, size_t len, u32 flags) {
    pty_t *pty = _handle_pty(handle);
    pty_queue_t *rx = _handle_rx_queue(pty, handle);

    if (!rx || !pty || !handle) {
        return -EINVAL;
    }

    bool nonblock = (flags & VFS_NONBLOCK) != 0;

    if (handle->is_master) {
        return _queue_read(rx, buf, len, nonblock);
    }

    return _queue_read_termios(rx, buf, len, nonblock, &pty->termios);
}

ssize_t pty_write_handle(
    const pty_handle_t *handle,
    const void *buf,
    size_t len,
    u32 flags
) {
    pty_t *pty = _handle_pty(handle);
    pty_queue_t *tx = _handle_tx_queue(pty, handle);

    if (!tx) {
        return -EINVAL;
    }

    return _queue_write(tx, buf, len, (flags & VFS_NONBLOCK) != 0);
}

ssize_t pty_ioctl_handle(const pty_handle_t *handle, u64 request, void *args) {
    pty_t *pty = _handle_pty(handle);
    if (!pty) {
        return -EINVAL;
    }

    switch (request) {
    case TCGETS:
        if (!args) {
            return -EINVAL;
        }

        memcpy(args, &pty->termios, sizeof(pty->termios));
        return 0;
    case TCSETS:
        if (!args) {
            return -EINVAL;
        }

        memcpy(&pty->termios, args, sizeof(pty->termios));
        return 0;
    case TCSETSW:
        if (!args) {
            return -EINVAL;
        }

        memcpy(&pty->termios, args, sizeof(pty->termios));
        return 0;
    case TCSETSF:
        if (!args) {
            return -EINVAL;
        }

        memcpy(&pty->termios, args, sizeof(pty->termios));
        _queue_clear(&pty->master_rx);
        _queue_clear(&pty->slave_rx);
        sched_wake_all(&pty->master_rx.write_wait);
        sched_wake_all(&pty->slave_rx.write_wait);
        return 0;
    case TIOCGWINSZ:
        if (!args) {
            return -EINVAL;
        }

        memcpy(args, &pty->winsize, sizeof(pty->winsize));
        return 0;
    case TIOCSWINSZ: {
        if (!args) {
            return -EINVAL;
        }

        pid_t winch_pgrp = 0;
        unsigned long irq_flags = spin_lock_irqsave(&pty_state_lock);
        winsize_t old_ws = pty->winsize;
        memcpy(&pty->winsize, args, sizeof(pty->winsize));

        if (
            pty->pgrp > 0 &&
            (
                old_ws.ws_row != pty->winsize.ws_row ||
                old_ws.ws_col != pty->winsize.ws_col
            )
        ) {
            winch_pgrp = pty->pgrp;
        }
        spin_unlock_irqrestore(&pty_state_lock, irq_flags);

        if (winch_pgrp > 0) {
            sched_signal_send_pgrp(winch_pgrp, SIGWINCH);
        }

        return 0;
    }
    case TIOCSPGRP: {
        if (!args) {
            return -EINVAL;
        }

        if (*(pid_t *)args <= 0) {
            return -EINVAL;
        }

        sched_thread_t *current = sched_current();
        if (!current || !current->user_thread) {
            return -EPERM;
        }

        if (!sched_pgrp_in_session(*(pid_t *)args, current->sid)) {
            return -EPERM;
        }

        unsigned long pty_flags = spin_lock_irqsave(&pty_state_lock);
        if (!pty->allocated) {
            spin_unlock_irqrestore(&pty_state_lock, pty_flags);
            return -EIO;
        }
        pty->pgrp = *(pid_t *)args;
        spin_unlock_irqrestore(&pty_state_lock, pty_flags);
        return 0;
    }
    case TIOCGPGRP: {
        if (!args) {
            return -EINVAL;
        }

        unsigned long pty_flags = spin_lock_irqsave(&pty_state_lock);
        *(pid_t *)args = pty->pgrp;
        spin_unlock_irqrestore(&pty_state_lock, pty_flags);
        return 0;
    }
    case TIOCGPTN:
        if (!args || !handle) {
            return -EINVAL;
        }

        *(int *)args = (int)handle->index;
        return 0;
    default:
        return -ENOTTY;
    }
}

short pty_poll_handle(const pty_handle_t *handle, short events, u32 flags) {
    (void)flags;

    pty_t *pty = _handle_pty(handle);
    if (!pty) {
        return POLLNVAL;
    }

    pty_queue_t *rx = _handle_rx_queue(pty, handle);
    pty_queue_t *tx = _handle_tx_queue(pty, handle);

    if (!rx || !tx) {
        return POLLNVAL;
    }

    short revents = 0;

    if ((events & POLLIN) && _queue_size(rx)) {
        revents |= POLLIN;
    }

    if ((events & POLLOUT) && _queue_free_space(tx)) {
        revents |= POLLOUT;
    }

    if (!pty->allocated) {
        revents |= POLLHUP;
    }

    return revents;
}
