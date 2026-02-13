#include "pty.h"

#include <arch/arch.h>
#include <errno.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <string.h>
#include <sys/ioctl.h>

#include "vfs.h"

typedef struct {
    u8 data[PTY_BUFFER_SIZE];
    size_t read_pos;
    size_t write_pos;
    size_t size;
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

static bool _handle_valid(const pty_handle_t* handle) {
    if (!handle)
        return false;

    return handle->index < PTY_COUNT;
}

static pty_t* _handle_pty(const pty_handle_t* handle) {
    if (!_handle_valid(handle))
        return NULL;

    return &ptys[handle->index];
}

static pty_queue_t* _handle_rx_queue(pty_t* pty, const pty_handle_t* handle) {
    if (!pty || !handle)
        return NULL;

    if (handle->is_master)
        return &pty->master_rx;

    return &pty->slave_rx;
}

static pty_queue_t* _handle_tx_queue(pty_t* pty, const pty_handle_t* handle) {
    if (!pty || !handle)
        return NULL;

    if (handle->is_master)
        return &pty->slave_rx;

    return &pty->master_rx;
}

static void _queue_init(pty_queue_t* queue) {
    if (!queue || queue->ready)
        return;

    queue->read_pos = 0;
    queue->write_pos = 0;
    queue->size = 0;
    sched_wait_queue_init(&queue->read_wait);
    sched_wait_queue_init(&queue->write_wait);
    queue->ready = true;
}

static size_t _queue_read_once(pty_queue_t* queue, void* buf, size_t len) {
    if (!queue || !buf || !len || !queue->size)
        return 0;

    size_t chunk = len;
    if (chunk > queue->size)
        chunk = queue->size;

    size_t first = chunk;
    if (first > PTY_BUFFER_SIZE - queue->read_pos)
        first = PTY_BUFFER_SIZE - queue->read_pos;

    memcpy(buf, queue->data + queue->read_pos, first);

    if (chunk > first)
        memcpy((u8*)buf + first, queue->data, chunk - first);

    queue->read_pos = (queue->read_pos + chunk) % PTY_BUFFER_SIZE;
    queue->size -= chunk;

    return chunk;
}

static size_t _queue_write_once(pty_queue_t* queue, const void* buf, size_t len) {
    if (!queue || !buf || !len)
        return 0;

    size_t free_space = PTY_BUFFER_SIZE - queue->size;
    if (!free_space)
        return 0;

    size_t chunk = len;
    if (chunk > free_space)
        chunk = free_space;

    size_t first = chunk;
    if (first > PTY_BUFFER_SIZE - queue->write_pos)
        first = PTY_BUFFER_SIZE - queue->write_pos;

    memcpy(queue->data + queue->write_pos, buf, first);

    if (chunk > first)
        memcpy(queue->data, (const u8*)buf + first, chunk - first);

    queue->write_pos = (queue->write_pos + chunk) % PTY_BUFFER_SIZE;
    queue->size += chunk;

    return chunk;
}

static ssize_t _queue_read(pty_queue_t* queue, void* buf, size_t len, bool nonblock) {
    if (!queue || !buf)
        return -EINVAL;

    if (!len)
        return 0;

    size_t total = 0;

    for (;;) {
        unsigned long irq_flags = arch_irq_save();
        size_t read_now = _queue_read_once(queue, (u8*)buf + total, len - total);
        arch_irq_restore(irq_flags);

        if (read_now) {
            total += read_now;
            sched_wake_one(&queue->write_wait);

            if (total == len)
                return (ssize_t)total;
        }

        if (total)
            return (ssize_t)total;

        if (nonblock)
            return -EAGAIN;

        if (!sched_is_running())
            continue;

        sched_thread_t* current = sched_current();
        if (current && sched_signal_has_pending(current))
            return -EINTR;

        sched_block(&queue->read_wait);
    }
}

static ssize_t _queue_write(pty_queue_t* queue, const void* buf, size_t len, bool nonblock) {
    if (!queue || !buf)
        return -EINVAL;

    if (!len)
        return 0;

    size_t total = 0;

    for (;;) {
        unsigned long irq_flags = arch_irq_save();
        size_t wrote_now = _queue_write_once(queue, (const u8*)buf + total, len - total);
        arch_irq_restore(irq_flags);

        if (wrote_now) {
            total += wrote_now;
            sched_wake_one(&queue->read_wait);

            if (total == len)
                return (ssize_t)total;
        }

        if (total == len)
            return (ssize_t)total;

        if (nonblock)
            return total ? (ssize_t)total : -EAGAIN;

        if (!sched_is_running())
            continue;

        sched_thread_t* current = sched_current();
        if (current && sched_signal_has_pending(current))
            return total ? (ssize_t)total : -EINTR;

        sched_block(&queue->write_wait);
    }
}

void pty_init(void) {
    for (size_t i = 0; i < PTY_COUNT; i++) {
        pty_t* pty = &ptys[i];

        _queue_init(&pty->master_rx);
        _queue_init(&pty->slave_rx);
        __termios_default_init(&pty->termios);

        pty->winsize.ws_col = 80;
        pty->winsize.ws_row = 25;
        pty->winsize.ws_xpixel = 0;
        pty->winsize.ws_ypixel = 0;
        pty->pgrp = 0;
        pty->allocated = false;
        pty->refs = 0;
    }
}

bool pty_reserve(size_t* index_out) {
    if (!index_out)
        return false;

    unsigned long irq_flags = arch_irq_save();

    for (size_t i = 0; i < PTY_COUNT; i++) {
        pty_t* pty = &ptys[i];
        if (pty->allocated)
            continue;

        pty->allocated = true;
        pty->refs = 0;
        *index_out = i;

        arch_irq_restore(irq_flags);
        return true;
    }

    arch_irq_restore(irq_flags);
    return false;
}

void pty_unreserve(size_t index) {
    if (index >= PTY_COUNT)
        return;

    unsigned long irq_flags = arch_irq_save();
    pty_t* pty = &ptys[index];

    if (pty->allocated && !pty->refs)
        pty->allocated = false;

    arch_irq_restore(irq_flags);
}

void pty_hold(size_t index) {
    if (index >= PTY_COUNT)
        return;

    unsigned long irq_flags = arch_irq_save();
    pty_t* pty = &ptys[index];

    if (pty->allocated)
        pty->refs++;

    arch_irq_restore(irq_flags);
}

void pty_put(size_t index) {
    if (index >= PTY_COUNT)
        return;

    unsigned long irq_flags = arch_irq_save();
    pty_t* pty = &ptys[index];

    if (pty->allocated && pty->refs) {
        pty->refs--;

        if (!pty->refs)
            pty->allocated = false;
    }

    arch_irq_restore(irq_flags);
}

ssize_t pty_read_handle(const pty_handle_t* handle, void* buf, size_t len, u32 flags) {
    pty_t* pty = _handle_pty(handle);
    pty_queue_t* rx = _handle_rx_queue(pty, handle);

    if (!rx)
        return -EINVAL;

    return _queue_read(rx, buf, len, (flags & VFS_NONBLOCK) != 0);
}

ssize_t pty_write_handle(const pty_handle_t* handle, const void* buf, size_t len, u32 flags) {
    pty_t* pty = _handle_pty(handle);
    pty_queue_t* tx = _handle_tx_queue(pty, handle);

    if (!tx)
        return -EINVAL;

    return _queue_write(tx, buf, len, (flags & VFS_NONBLOCK) != 0);
}

ssize_t pty_ioctl_handle(const pty_handle_t* handle, u64 request, void* args) {
    pty_t* pty = _handle_pty(handle);
    if (!pty)
        return -EINVAL;

    switch (request) {
    case TCGETS:
        if (!args)
            return -EINVAL;

        memcpy(args, &pty->termios, sizeof(pty->termios));
        return 0;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        if (!args)
            return -EINVAL;

        memcpy(&pty->termios, args, sizeof(pty->termios));
        return 0;
    case TIOCGWINSZ:
        if (!args)
            return -EINVAL;

        memcpy(args, &pty->winsize, sizeof(pty->winsize));
        return 0;
    case TIOCSWINSZ:
        if (!args)
            return -EINVAL;

        memcpy(&pty->winsize, args, sizeof(pty->winsize));
        return 0;
    case TIOCSPGRP:
        if (!args)
            return -EINVAL;

        pty->pgrp = *(pid_t*)args;
        return 0;
    case TIOCGPGRP:
        if (!args)
            return -EINVAL;

        *(pid_t*)args = pty->pgrp;
        return 0;
    case TIOCGPTN:
        if (!args || !handle)
            return -EINVAL;

        *(int*)args = (int)handle->index;
        return 0;
    default:
        return -ENOTTY;
    }
}
