#include "pty.h"

#include <arch/arch.h>
#include <data/ring.h>
#include <errno.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <signal.h>
#include <stdio.h>
#include <sys/devfs.h>
#include <sys/ioctl.h>
#include <sys/usercopy.h>

#include "vfs.h"

typedef struct {
    u8 data[PTY_BUFFER_SIZE];
    ring_io_t ring;
    spinlock_t lock;
    sched_wait_queue_t read_wait;
    sched_wait_queue_t write_wait;
    bool ready;
    bool closed;
} pty_queue_t;

typedef struct {
    pty_queue_t master_rx;
    pty_queue_t slave_rx;
    termios_t termios;
    winsize_t winsize;
    pid_t pgrp;
    bool allocated;
    bool closing;
    size_t refs;
    size_t master_refs;
    size_t slave_refs;
} pty_t;

typedef struct {
    pty_t ptys[PTY_COUNT];
    pty_handle_t slave_handles[PTY_COUNT];
    vfs_interface_t *interface;
    spinlock_t lock;
} pty_state_t;

static pty_state_t pty_state = {
    .lock = SPINLOCK_INIT,
};

static void _queue_reset(pty_queue_t *queue) {
    if (!queue) {
        return;
    }

    ring_io_init(&queue->ring, queue->data, PTY_BUFFER_SIZE);
    queue->closed = false;
}

static void _reset_state(pty_t *pty) {
    if (!pty) {
        return;
    }

    _queue_reset(&pty->master_rx);
    _queue_reset(&pty->slave_rx);
    termios_init_default(&pty->termios);

    pty->winsize.ws_col = 80;
    pty->winsize.ws_row = 25;
    pty->winsize.ws_xpixel = 0;
    pty->winsize.ws_ypixel = 0;
    pty->pgrp = 0;
    pty->closing = false;
    pty->refs = 0;
    pty->master_refs = 0;
    pty->slave_refs = 0;
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

    return &pty_state.ptys[handle->index];
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

    sched_waitq_init(&queue->read_wait);
    sched_waitq_init(&queue->write_wait);
    sched_waitq_set_poll(&queue->read_wait, true);
    sched_waitq_set_poll(&queue->write_wait, true);

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

static bool _queue_closed(pty_queue_t *queue) {
    if (!queue) {
        return true;
    }

    unsigned long irq_flags = spin_lock_irqsave(&queue->lock);
    bool closed = queue->closed;
    spin_unlock_irqrestore(&queue->lock, irq_flags);

    return closed;
}

static void _queue_close(pty_queue_t *queue) {
    if (!queue) {
        return;
    }

    unsigned long irq_flags = spin_lock_irqsave(&queue->lock);
    queue->closed = true;
    spin_unlock_irqrestore(&queue->lock, irq_flags);

    sched_wake_all(&queue->read_wait);
    sched_wake_all(&queue->write_wait);
}

static void _close_queues(pty_t *pty) {
    if (!pty) {
        return;
    }

    _queue_close(&pty->master_rx);
    _queue_close(&pty->slave_rx);
}

static size_t _queue_free_space(pty_queue_t *queue) {
    if (!queue) {
        return 0;
    }

    unsigned long irq_flags = spin_lock_irqsave(&queue->lock);
    size_t free_space = queue->closed ? 0 : ring_io_free_space(&queue->ring);
    spin_unlock_irqrestore(&queue->lock, irq_flags);

    return free_space;
}

static void _seed_handles(void) {
    for (size_t i = 0; i < PTY_COUNT; i++) {
        pty_state.slave_handles[i].index = i;
        pty_state.slave_handles[i].is_master = false;
    }
}

static ssize_t _dev_pty_read(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)offset;
    return pty_read_handle(node ? node->private : NULL, buf, len, flags);
}

static ssize_t _dev_pty_write(vfs_node_t *node, void *buf, size_t offset, size_t len, u32 flags) {
    (void)offset;
    return pty_write_handle(node ? node->private : NULL, buf, len, flags);
}

static ssize_t _dev_pty_ioctl(vfs_node_t *node, u64 request, void *args) {
    return pty_ioctl_handle(node ? node->private : NULL, request, args);
}

static short _dev_pty_poll(vfs_node_t *node, short events, u32 flags) {
    return pty_poll_handle(node ? node->private : NULL, events, flags);
}

static sched_wait_queue_t *handle_wait_queue(const pty_handle_t *handle, short events, u32 flags) {
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

static sched_wait_queue_t *pty_wait_queue(vfs_node_t *node, short events, u32 flags) {
    return handle_wait_queue(node ? node->private : NULL, events, flags);
}

static bool ensure_interface(void) {
    if (pty_state.interface) {
        return true;
    }

    pty_state.interface = vfs_create_interface(_dev_pty_read, _dev_pty_write, NULL);

    if (!pty_state.interface) {
        return false;
    }

    pty_state.interface->ioctl = _dev_pty_ioctl;
    pty_state.interface->poll = _dev_pty_poll;
    pty_state.interface->wait_queue = pty_wait_queue;

    return true;
}

static bool _register_slave(size_t index) {
    if (index >= PTY_COUNT || !devfs_is_ready()) {
        return true;
    }

    if (!ensure_interface()) {
        return false;
    }

    vfs_node_t *dev_dir = vfs_lookup("/dev");
    if (!dev_dir) {
        return false;
    }

    char name[16];
    snprintf(name, sizeof(name), "pts%zu", index);

    pty_handle_t *handle = &pty_state.slave_handles[index];
    return devfs_register_node(dev_dir, name, VFS_CHARDEV, 0666, pty_state.interface, handle);
}

static void _unregister_slave(size_t index) {
    if (index >= PTY_COUNT || !devfs_is_ready()) {
        return;
    }

    char path[24];
    snprintf(path, sizeof(path), "/dev/pts%zu", index);

    if (!devfs_unregister_node(path)) {
        return;
    }
}

static void _release_if_idle(size_t index) {
    if (index >= PTY_COUNT) {
        return;
    }

    pid_t hup_pgrp = 0;
    bool should_release = false;

    unsigned long irq_flags = spin_lock_irqsave(&pty_state.lock);
    pty_t *pty = &pty_state.ptys[index];

    if (pty->allocated && !pty->refs && !pty->closing) {
        pty->closing = true;
        hup_pgrp = pty->pgrp;
        should_release = true;
    }

    spin_unlock_irqrestore(&pty_state.lock, irq_flags);

    if (!should_release) {
        return;
    }

    _unregister_slave(index);

    irq_flags = spin_lock_irqsave(&pty_state.lock);

    if (pty->allocated && !pty->refs) {
        _reset_state(pty);
        pty->allocated = false;
    } else if (pty->allocated) {
        pty->closing = false;
    }

    spin_unlock_irqrestore(&pty_state.lock, irq_flags);

    if (hup_pgrp > 0) {
        sched_signal_send_pgrp(hup_pgrp, SIGHUP);
        sched_signal_send_pgrp(hup_pgrp, SIGCONT);
    }
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

static sched_wait_result_t _pty_wait(sched_wait_queue_t *queue, u32 seq, u64 deadline) {
    return sched_wait_on(queue, seq, deadline, SCHED_WAIT_INTERRUPTIBLE);
}

static ssize_t _queue_result(size_t total, int error) {
    if (total) {
        return (ssize_t)total;
    }

    return -error;
}

static ssize_t _queue_read(pty_queue_t *queue, void *buf, size_t len, bool nonblock) {
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
        bool closed = queue->closed;

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

        if (closed) {
            return 0;
        }

        if (nonblock) {
            return -EAGAIN;
        }

        if (!sched_is_running()) {
            continue;
        }

        sched_thread_t *current = sched_current();

        if (current && sched_signal_pending(current)) {
            return -EINTR;
        }

        sched_wait_result_t wait_result = _pty_wait(&queue->read_wait, wait_seq, 0);
        if (wait_result == SCHED_WAIT_INTR) {
            return -EINTR;
        }
    }
}

static bool _termios_read_enough(size_t total, size_t len, size_t vmin, size_t target) {
    if (total == len) {
        return true;
    }

    if (!vmin) {
        return true;
    }

    return total >= target;
}

static bool _termios_timeout_expired(u64 timeout_ticks, u64 *deadline) {
    if (!timeout_ticks) {
        return false;
    }

    if (!*deadline) {
        *deadline = arch_timer_ticks() + timeout_ticks;
    }

    return arch_timer_ticks() >= *deadline;
}

static bool _termios_wait_has_timeout(size_t vmin, u64 timeout_ticks, bool seen_first_byte) {
    if (!timeout_ticks) {
        return false;
    }

    return !vmin || seen_first_byte;
}

static bool
_termios_idle_done(size_t total, bool closed, size_t vmin, u64 timeout_ticks, u64 *deadline, ssize_t *result) {
    if (total) {
        if (!vmin || closed || _termios_timeout_expired(timeout_ticks, deadline)) {
            *result = (ssize_t)total;
            return true;
        }

        return false;
    }

    if (closed) {
        *result = 0;
        return true;
    }

    if (!vmin && !timeout_ticks) {
        *result = 0;
        return true;
    }

    if (!vmin && _termios_timeout_expired(timeout_ticks, deadline)) {
        *result = 0;
        return true;
    }

    return false;
}

static sched_wait_result_t _termios_wait(pty_queue_t *queue, u32 wait_seq, bool timed, u64 deadline) {
    if (timed) {
        return _pty_wait(&queue->read_wait, wait_seq, deadline);
    }

    return sched_wait_on(&queue->read_wait, wait_seq, 0, SCHED_WAIT_INTERRUPTIBLE);
}

static size_t _queue_take_locked(pty_queue_t *queue, u8 *buf, size_t len, bool *closed_out) {
    unsigned long irq_flags = spin_lock_irqsave(&queue->lock);

    size_t read_now = ring_io_read(&queue->ring, buf, len);
    bool closed = queue->closed;

    spin_unlock_irqrestore(&queue->lock, irq_flags);

    if (closed_out) {
        *closed_out = closed;
    }

    return read_now;
}

static bool _termios_note_bytes(
    size_t *total,
    bool *seen_byte,
    u64 *deadline,
    size_t got,
    size_t limit,
    size_t vmin,
    size_t target,
    u64 timeout_ticks
) {
    *total += got;
    *seen_byte = true;

    if (_termios_read_enough(*total, limit, vmin, target)) {
        return true;
    }

    if (timeout_ticks) {
        *deadline = arch_timer_ticks() + timeout_ticks;
    }

    return false;
}

static ssize_t _queue_read_termios(pty_queue_t *queue, void *buf, size_t len, bool nonblock, const termios_t *tos) {
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
        bool closed = false;
        size_t read_now = _queue_take_locked(queue, (u8 *)buf + total, len - total, &closed);

        if (read_now) {
            sched_wake_one(&queue->write_wait);

            if (_termios_note_bytes(&total, &first_byte_seen, &deadline, read_now, len, vmin, target, timeout_ticks)) {
                return (ssize_t)total;
            }

            continue;
        }

        ssize_t idle_result = 0;
        if (_termios_idle_done(total, closed, vmin, timeout_ticks, &deadline, &idle_result)) {
            return idle_result;
        }

        if (nonblock) {
            return _queue_result(total, EAGAIN);
        }

        if (!sched_is_running()) {
            arch_cpu_wait();
            continue;
        }

        sched_thread_t *current = sched_current();
        if (current && sched_signal_pending(current)) {
            return _queue_result(total, EINTR);
        }

        bool timed = _termios_wait_has_timeout(vmin, timeout_ticks, first_byte_seen);
        sched_wait_result_t wait_result = _termios_wait(queue, wait_seq, timed, deadline);

        if (wait_result == SCHED_WAIT_TIMEOUT) {
            return (ssize_t)total;
        }
        if (wait_result == SCHED_WAIT_INTR) {
            return _queue_result(total, EINTR);
        }
    }
}

static ssize_t _queue_write(pty_queue_t *queue, const void *buf, size_t len, bool nonblock) {
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

        bool closed = queue->closed;
        size_t wrote_now = closed ? 0 : ring_io_write(&queue->ring, (const u8 *)buf + total, len - total);

        spin_unlock_irqrestore(&queue->lock, irq_flags);

        if (closed) {
            return _queue_result(total, EIO);
        }

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
            return _queue_result(total, EAGAIN);
        }

        if (!sched_is_running()) {
            continue;
        }

        sched_thread_t *current = sched_current();
        if (current && sched_signal_pending(current)) {
            return _queue_result(total, EINTR);
        }

        sched_wait_result_t wait_result = _pty_wait(&queue->write_wait, wait_seq, 0);

        if (wait_result == SCHED_WAIT_INTR) {
            return _queue_result(total, EINTR);
        }
    }
}

static bool pty_register_devfs(vfs_node_t *dev_dir) {
    if (!dev_dir) {
        return false;
    }

    if (PTY_COUNT && !pty_state.ptys[0].master_rx.ready) {
        log_warn("pty state not ready");
        return false;
    }

    _seed_handles();

    if (!ensure_interface()) {
        log_warn("failed to allocate pty devfs interface");
        return false;
    }

    bool ptmx_registered = devfs_register_node(dev_dir, "ptmx", VFS_CHARDEV, 0666, pty_state.interface, NULL);

    if (!ptmx_registered) {
        log_warn("failed to create /dev/ptmx");
        return false;
    }

    return true;
}

void pty_init(void) {
    if (!devfs_register_device("pty", pty_register_devfs)) {
        log_warn("failed to register devfs init callback");
    }

    if (PTY_COUNT && pty_state.ptys[0].master_rx.ready) {
        return;
    }

    for (size_t i = 0; i < PTY_COUNT; i++) {
        pty_t *pty = &pty_state.ptys[i];

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

    unsigned long irq_flags = spin_lock_irqsave(&pty_state.lock);

    for (size_t i = 0; i < PTY_COUNT; i++) {
        pty_t *pty = &pty_state.ptys[i];

        if (pty->allocated) {
            continue;
        }

        _reset_state(pty);
        pty->allocated = true;
        pty->closing = false;
        *index_out = i;

        spin_unlock_irqrestore(&pty_state.lock, irq_flags);

        if (!_register_slave(i)) {
            pty_unreserve(i);
            return false;
        }

        return true;
    }

    spin_unlock_irqrestore(&pty_state.lock, irq_flags);
    return false;
}

void pty_unreserve(size_t index) {
    if (index >= PTY_COUNT) {
        return;
    }

    _release_if_idle(index);
}

void pty_hold(size_t index, bool master) {
    if (index >= PTY_COUNT) {
        return;
    }

    unsigned long irq_flags = spin_lock_irqsave(&pty_state.lock);

    pty_t *pty = &pty_state.ptys[index];

    if (pty->allocated) {
        pty->refs++;
        if (master) {
            pty->master_refs++;
        } else {
            pty->slave_refs++;
        }
    }

    spin_unlock_irqrestore(&pty_state.lock, irq_flags);
}

void pty_put(size_t index, bool master) {
    if (index >= PTY_COUNT) {
        return;
    }

    unsigned long irq_flags = spin_lock_irqsave(&pty_state.lock);
    pty_t *pty = &pty_state.ptys[index];
    bool idle = false;
    bool hangup = false;
    pid_t hup_pgrp = 0;

    if (pty->allocated && pty->refs) {
        pty->refs--;

        if (master && pty->master_refs) {
            pty->master_refs--;
            hangup = pty->master_refs == 0 && pty->slave_refs != 0;
        } else if (!master && pty->slave_refs) {
            pty->slave_refs--;
            hangup = pty->slave_refs == 0 && pty->master_refs != 0;
        }

        if (hangup) {
            hup_pgrp = pty->pgrp;
        }

        if (!pty->refs) {
            idle = true;
        }
    }

    spin_unlock_irqrestore(&pty_state.lock, irq_flags);

    if (hangup) {
        _close_queues(pty);

        if (hup_pgrp > 0) {
            sched_signal_send_pgrp(hup_pgrp, SIGHUP);
            sched_signal_send_pgrp(hup_pgrp, SIGCONT);
        }
    }

    if (idle) {
        _release_if_idle(index);
    }
}

ssize_t pty_read_handle(const pty_handle_t *handle, void *buf, size_t len, u32 flags) {
    pty_t *pty = _handle_pty(handle);
    pty_queue_t *rx = _handle_rx_queue(pty, handle);

    if (!rx || !pty || !handle) {
        return -EINVAL;
    }

    bool nonblock = (flags & VFS_NONBLOCK) != 0;

    if (handle->is_master) {
        return _queue_read(rx, buf, len, nonblock);
    }

    unsigned long pty_flags = spin_lock_irqsave(&pty_state.lock);
    termios_t tos = pty->termios;
    spin_unlock_irqrestore(&pty_state.lock, pty_flags);

    return _queue_read_termios(rx, buf, len, nonblock, &tos);
}

ssize_t pty_write_handle(const pty_handle_t *handle, const void *buf, size_t len, u32 flags) {
    pty_t *pty = _handle_pty(handle);
    pty_queue_t *tx = _handle_tx_queue(pty, handle);

    if (!tx) {
        return -EINVAL;
    }

    return _queue_write(tx, buf, len, (flags & VFS_NONBLOCK) != 0);
}

static int _pty_get_termios(pty_t *pty, void *args) {
    if (!args) {
        return -EINVAL;
    }

    unsigned long pty_flags = spin_lock_irqsave(&pty_state.lock);
    termios_t tos = pty->termios;
    spin_unlock_irqrestore(&pty_state.lock, pty_flags);

    sched_thread_t *current = sched_current();
    if (!user_copy_to(current, args, &tos, sizeof(tos))) {
        return -EFAULT;
    }

    return 0;
}

static int _pty_set_termios(pty_t *pty, u64 request, void *args) {
    if (!args) {
        return -EINVAL;
    }

    sched_thread_t *current = sched_current();
    termios_t tos = { 0 };
    if (!user_copy_from(current, &tos, args, sizeof(tos))) {
        return -EFAULT;
    }

    unsigned long pty_flags = spin_lock_irqsave(&pty_state.lock);
    pty->termios = tos;
    spin_unlock_irqrestore(&pty_state.lock, pty_flags);

    if (request == TCSETSF) {
        _queue_clear(&pty->master_rx);
        _queue_clear(&pty->slave_rx);
        sched_wake_all(&pty->master_rx.write_wait);
        sched_wake_all(&pty->slave_rx.write_wait);
    }

    return 0;
}

static int _pty_get_winsize(pty_t *pty, void *args) {
    if (!args) {
        return -EINVAL;
    }

    unsigned long irq_flags = spin_lock_irqsave(&pty_state.lock);
    winsize_t winsize = pty->winsize;
    spin_unlock_irqrestore(&pty_state.lock, irq_flags);

    sched_thread_t *current = sched_current();
    if (!user_copy_to(current, args, &winsize, sizeof(winsize))) {
        return -EFAULT;
    }

    return 0;
}

static bool winsize_changed(const winsize_t *before, const winsize_t *after) {
    return before->ws_row != after->ws_row || before->ws_col != after->ws_col;
}

static int _pty_set_winsize(pty_t *pty, void *args) {
    if (!args) {
        return -EINVAL;
    }

    sched_thread_t *current = sched_current();
    winsize_t winsize = { 0 };
    if (!user_copy_from(current, &winsize, args, sizeof(winsize))) {
        return -EFAULT;
    }

    pid_t winch_pgrp = 0;
    unsigned long irq_flags = spin_lock_irqsave(&pty_state.lock);
    winsize_t old_ws = pty->winsize;
    pty->winsize = winsize;

    if (pty->pgrp > 0 && winsize_changed(&old_ws, &pty->winsize)) {
        winch_pgrp = pty->pgrp;
    }

    spin_unlock_irqrestore(&pty_state.lock, irq_flags);

    if (winch_pgrp > 0) {
        sched_signal_send_pgrp(winch_pgrp, SIGWINCH);
    }

    return 0;
}

static int _pty_set_pgrp(const pty_handle_t *handle, pty_t *pty, void *args) {
    if (!args) {
        return -EINVAL;
    }

    sched_thread_t *current = sched_current();
    pid_t pgid = 0;
    if (!user_copy_from(current, &pgid, args, sizeof(pgid))) {
        return -EFAULT;
    }

    if (pgid <= 0) {
        return -EINVAL;
    }

    if (!current || !current->user_thread) {
        return -EPERM;
    }

    bool slave = handle && !handle->is_master;
    bool foreground_tty = slave && current->tty_index == PROC_TTY_PTS((int)handle->index);
    if (!foreground_tty) {
        return -ENOTTY;
    }

    if (!sched_pgrp_in_session(pgid, current->sid)) {
        return -EPERM;
    }

    unsigned long pty_flags = spin_lock_irqsave(&pty_state.lock);
    if (!pty->allocated) {
        spin_unlock_irqrestore(&pty_state.lock, pty_flags);
        return -EIO;
    }

    pty->pgrp = pgid;
    spin_unlock_irqrestore(&pty_state.lock, pty_flags);

    return 0;
}

static int _pty_get_pgrp(pty_t *pty, void *args) {
    if (!args) {
        return -EINVAL;
    }

    unsigned long pty_flags = spin_lock_irqsave(&pty_state.lock);
    pid_t pgrp = pty->pgrp;
    spin_unlock_irqrestore(&pty_state.lock, pty_flags);

    sched_thread_t *current = sched_current();
    if (!user_copy_to(current, args, &pgrp, sizeof(pgrp))) {
        return -EFAULT;
    }

    return 0;
}

static int _pty_get_index(const pty_handle_t *handle, void *args) {
    if (!args || !handle) {
        return -EINVAL;
    }

    int index = (int)handle->index;
    sched_thread_t *current = sched_current();
    if (!user_copy_to(current, args, &index, sizeof(index))) {
        return -EFAULT;
    }

    return 0;
}

ssize_t pty_ioctl_handle(const pty_handle_t *handle, u64 request, void *args) {
    pty_t *pty = _handle_pty(handle);
    if (!pty) {
        return -EINVAL;
    }

    switch (request) {
    case TCGETS:
        return _pty_get_termios(pty, args);
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        return _pty_set_termios(pty, request, args);
    case TIOCGWINSZ:
        return _pty_get_winsize(pty, args);
    case TIOCSWINSZ:
        return _pty_set_winsize(pty, args);
    case TIOCSPGRP:
        return _pty_set_pgrp(handle, pty, args);
    case TIOCGPGRP:
        return _pty_get_pgrp(pty, args);
    case TIOCGPTN:
        return _pty_get_index(handle, args);
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

    if ((events & POLLIN) && (_queue_size(rx) || _queue_closed(rx))) {
        revents |= POLLIN;
    }

    if ((events & POLLOUT) && _queue_free_space(tx)) {
        revents |= POLLOUT;
    }

    unsigned long irq_flags = spin_lock_irqsave(&pty_state.lock);
    bool pty_closed = !pty->allocated || pty->closing;
    spin_unlock_irqrestore(&pty_state.lock, irq_flags);

    if (pty_closed || _queue_closed(rx) || _queue_closed(tx)) {
        revents |= POLLHUP;
    }

    return revents;
}
