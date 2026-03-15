#include "scheduler.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/procfs.h>
#include <sys/pty.h>
#include <sys/tty.h>

static void sched_fd_reset(sched_fd_t *fd) {
    if (!fd) {
        return;
    }

    fd->kind = SCHED_FD_NONE;
    fd->node = NULL;
    fd->pipe = NULL;
    fd->offset = 0;
    fd->pty_index = -1;
    fd->tty_index = TTY_NONE;
    fd->flags = 0;
    fd->fd_flags = 0;
}

static void sched_pipe_try_destroy(sched_pipe_t *pipe) {
    if (!pipe) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&pipe->lock);
    bool in_use = pipe->readers || pipe->writers || pipe->wake_refs || pipe->destroying;

    if (!in_use) {
        pipe->destroying = true;
    }

    spin_unlock_irqrestore(&pipe->lock, flags);

    if (in_use) {
        return;
    }

    if (pipe->read_wait_owned && pipe->read_wait_queue) {
        sched_wait_queue_destroy(pipe->read_wait_queue);
    }

    if (pipe->write_wait_owned && pipe->write_wait_queue) {
        sched_wait_queue_destroy(pipe->write_wait_queue);
    }

    free(pipe->read_wait_queue);
    free(pipe->write_wait_queue);
    free(pipe->ring.data);
    free(pipe);
}

sched_pipe_t *sched_pipe_create(size_t capacity) {
    if (!capacity) {
        capacity = SCHED_PIPE_CAPACITY;
    }

    sched_pipe_t *pipe = calloc(1, sizeof(*pipe));
    if (!pipe) {
        return NULL;
    }

    u8 *data = calloc(capacity, sizeof(u8));

    pipe->read_wait_queue = calloc(1, sizeof(sched_wait_queue_t));
    pipe->write_wait_queue = calloc(1, sizeof(sched_wait_queue_t));

    if (!data || !pipe->read_wait_queue || !pipe->write_wait_queue) {
        free(data);
        free(pipe->read_wait_queue);
        free(pipe->write_wait_queue);
        free(pipe);
        return NULL;
    }

    ring_io_init(&pipe->ring, data, capacity);
    spinlock_init(&pipe->lock);
    pipe->wake_refs = 0;
    pipe->read_wait_owned = true;
    pipe->write_wait_owned = true;
    sched_wait_queue_init(pipe->read_wait_queue);
    sched_wait_queue_init(pipe->write_wait_queue);
    sched_wait_queue_set_poll_link(pipe->read_wait_queue, true);
    sched_wait_queue_set_poll_link(pipe->write_wait_queue, true);

    return pipe;
}

void sched_pipe_acquire_reader(sched_pipe_t *pipe) {
    if (!pipe) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&pipe->lock);
    pipe->readers++;
    spin_unlock_irqrestore(&pipe->lock, flags);
}

void sched_pipe_acquire_writer(sched_pipe_t *pipe) {
    if (!pipe) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&pipe->lock);
    pipe->writers++;
    spin_unlock_irqrestore(&pipe->lock, flags);
}

bool sched_pipe_operation_begin(sched_pipe_t *pipe) {
    if (!pipe) {
        return false;
    }

    unsigned long flags = spin_lock_irqsave(&pipe->lock);

    if (pipe->destroying) {
        spin_unlock_irqrestore(&pipe->lock, flags);
        return false;
    }

    pipe->wake_refs++;
    spin_unlock_irqrestore(&pipe->lock, flags);

    return true;
}

void sched_pipe_operation_end(sched_pipe_t *pipe) {
    if (!pipe) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&pipe->lock);

    if (pipe->wake_refs > 0) {
        pipe->wake_refs--;
    }

    spin_unlock_irqrestore(&pipe->lock, flags);

    sched_pipe_try_destroy(pipe);
}

static void _pipe_release(sched_pipe_t *pipe, bool is_reader) {
    if (!pipe) {
        return;
    }

    unsigned long flags = spin_lock_irqsave(&pipe->lock);

    if (is_reader) {
        if (pipe->readers > 0) {
            pipe->readers--;
        }
    } else {
        if (pipe->writers > 0) {
            pipe->writers--;
        }
    }

    pipe->wake_refs++;
    sched_wait_queue_t *read_wait = pipe->read_wait_queue;
    sched_wait_queue_t *write_wait = pipe->write_wait_queue;
    spin_unlock_irqrestore(&pipe->lock, flags);

    if (read_wait) {
        sched_wake_all(read_wait);
    }

    if (write_wait) {
        sched_wake_all(write_wait);
    }

    flags = spin_lock_irqsave(&pipe->lock);

    if (pipe->wake_refs > 0) {
        pipe->wake_refs--;
    }

    spin_unlock_irqrestore(&pipe->lock, flags);

    sched_pipe_try_destroy(pipe);
}

void sched_pipe_release_reader(sched_pipe_t *pipe) {
    _pipe_release(pipe, true);
}

void sched_pipe_release_writer(sched_pipe_t *pipe) {
    _pipe_release(pipe, false);
}

static void sched_fd_retain(const sched_fd_t *fd) {
    if (!fd) {
        return;
    }

    if (fd->pty_index >= 0) {
        pty_hold((size_t)fd->pty_index);
    }

    if (fd->kind == SCHED_FD_PIPE_READ) {
        sched_pipe_acquire_reader(fd->pipe);
    } else if (fd->kind == SCHED_FD_PIPE_WRITE) {
        sched_pipe_acquire_writer(fd->pipe);
    }
}

static void sched_fd_release_value(sched_fd_t *fd) {
    if (!fd) {
        return;
    }

    if (fd->pty_index >= 0) {
        pty_put((size_t)fd->pty_index);
    }

    if (fd->kind == SCHED_FD_PIPE_READ) {
        sched_pipe_release_reader(fd->pipe);
    } else if (fd->kind == SCHED_FD_PIPE_WRITE) {
        sched_pipe_release_writer(fd->pipe);
    }

    sched_fd_reset(fd);
}

int sched_fd_alloc(sched_thread_t *thread, const sched_fd_t *fd, int min_fd) {
    if (!thread || !fd || fd->kind == SCHED_FD_NONE) {
        return -EINVAL;
    }

    int start = min_fd < 0 ? 0 : min_fd;

    if (start >= SCHED_FD_MAX) {
        return -EMFILE;
    }

    for (int slot = start; slot < SCHED_FD_MAX; slot++) {
        if (thread->fd_used[slot]) {
            continue;
        }

        thread->fd_used[slot] = true;
        thread->fds[slot] = *fd;
        sched_fd_retain(&thread->fds[slot]);
        
        return slot;
    }

    return -EMFILE;
}

int sched_fd_close(sched_thread_t *thread, int fd) {
    if (!thread || fd < 0 || fd >= SCHED_FD_MAX || !thread->fd_used[fd]) {
        return -EBADF;
    }

    sched_fd_t old = thread->fds[fd];
    thread->fd_used[fd] = false;

    sched_fd_reset(&thread->fds[fd]);
    sched_fd_release_value(&old);

    if (old.kind == SCHED_FD_VFS) {
        procfs_sweep_dead();
    }

    return 0;
}

int sched_fd_install(
    sched_thread_t *thread,
    int target_fd,
    const sched_fd_t *fd
) {
    if (!thread || !fd || fd->kind == SCHED_FD_NONE) {
        return -EINVAL;
    }

    if (target_fd < 0 || target_fd >= SCHED_FD_MAX) {
        return -EBADF;
    }

    if (thread->fd_used[target_fd]) {
        sched_fd_close(thread, target_fd);
    }

    thread->fd_used[target_fd] = true;
    thread->fds[target_fd] = *fd;

    sched_fd_retain(&thread->fds[target_fd]);

    return target_fd;
}

int sched_fd_dup(sched_thread_t *thread, int oldfd, int newfd) {
    if (!thread || oldfd < 0 || oldfd >= SCHED_FD_MAX || !thread->fd_used[oldfd]) {
        return -EBADF;
    }

    if (newfd < 0 || newfd >= SCHED_FD_MAX) {
        return -EBADF;
    }

    if (oldfd == newfd) {
        return newfd;
    }

    sched_fd_t source = thread->fds[oldfd];
    return sched_fd_install(thread, newfd, &source);
}

bool sched_fd_clone_table(sched_thread_t *dst, const sched_thread_t *src) {
    if (!dst || !src) {
        return false;
    }

    for (int fd = 0; fd < SCHED_FD_MAX; fd++) {
        if (!src->fd_used[fd]) {
            continue;
        }

        dst->fd_used[fd] = true;
        dst->fds[fd] = src->fds[fd];
        sched_fd_retain(&dst->fds[fd]);
    }

    return true;
}

void sched_fd_close_all(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    for (int fd = 0; fd < SCHED_FD_MAX; fd++) {
        if (!thread->fd_used[fd]) {
            continue;
        }

        sched_fd_close(thread, fd);
    }
}

void sched_fd_close_cloexec(sched_thread_t *thread) {
    if (!thread) {
        return;
    }

    for (int fd = 0; fd < SCHED_FD_MAX; fd++) {
        if (!thread->fd_used[fd]) {
            continue;
        }

        if (!(thread->fds[fd].fd_flags & SCHED_FD_FLAG_CLOEXEC)) {
            continue;
        }

        sched_fd_close(thread, fd);
    }
}
