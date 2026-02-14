#include "syscall.h"

#include <arch/arch.h>
#include <arch/mm.h>
#include <arch/paging.h>
#include <arch/syscall.h>
#include <arch/thread.h>
#include <base/macros.h>
#include <data/list.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <poll.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cpu.h>
#include <sys/exec.h>
#include <sys/mman.h>
#include <sys/path.h>
#include <sys/proc.h>
#include <sys/pty.h>
#include <sys/stat.h>
#include <sys/tty.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#define SYSCALL_INT 0x80

static mode_t _apply_umask(mode_t mode, mode_t mask) {
    mode_t special = mode & 07000;
    mode_t perms = (mode & 0777) & ~(mask & 0777);
    return special | perms;
}

static bool _open_has_read(int flags) {
    return (flags & O_RDONLY) || (flags & O_RDWR);
}

static bool _open_has_write(int flags) {
    return (flags & O_WRONLY) || (flags & O_RDWR);
}

static bool _open_access_valid(int flags) {
    int access = flags & (O_RDONLY | O_WRONLY | O_RDWR);

    return access == O_RDONLY || access == O_WRONLY || access == O_RDWR;
}

static int _open_fail(bool ptmx_open, size_t ptmx_index, int error) {
    if (ptmx_open)
        pty_unreserve(ptmx_index);

    return error;
}

static bool _parse_index(const char* text, int max, int* out_index) {
    if (!text || !out_index || !text[0] || max <= 0)
        return false;

    int value = 0;

    for (const char* p = text; *p; p++) {
        if (*p < '0' || *p > '9')
            return false;

        value = value * 10 + (*p - '0');

        if (value >= max)
            return false;
    }

    *out_index = value;
    return true;
}

static int _tty_binding_from_dev_name(const char* dev_name) {
    if (!dev_name || !dev_name[0])
        return TTY_NONE;

    if (!strcmp(dev_name, "tty"))
        return (int)tty_current_screen();

    if (!strcmp(dev_name, "console"))
        return TTY_CONSOLE;

    int idx = 0;

    if (!strncmp(dev_name, "tty", 3) && _parse_index(dev_name + 3, (int)TTY_COUNT, &idx))
        return (int)TTY_USER_TO_SCREEN(idx);

    if (!strncmp(dev_name, "pts", 3) && _parse_index(dev_name + 3, (int)PTY_COUNT, &idx))
        return PROC_TTY_PTS(idx);

    if (!strncmp(dev_name, "pty", 3) && _parse_index(dev_name + 3, (int)PTY_COUNT, &idx))
        return PROC_TTY_PTS(idx);

    return TTY_NONE;
}

static void _sync_thread_tty_from_entry(sched_thread_t* thread, const sched_fd_t* entry) {
    if (!thread || !entry)
        return;

    if (entry->tty_index != TTY_NONE)
        thread->tty_index = entry->tty_index;
}

static int
_enforce_sticky_delete(const sched_thread_t* thread, vfs_node_t* parent, vfs_node_t* target) {
    if (!thread || !parent || !target)
        return -EINVAL;

    if (!(parent->mode & S_ISVTX))
        return 0;

    if (!thread->uid || thread->uid == parent->uid || thread->uid == target->uid)
        return 0;

    return -EPERM;
}

static void _maybe_clear_setid(sched_thread_t* thread, vfs_node_t* node) {
    if (!thread || !node || !thread->uid)
        return;

    if (VFS_IS_LINK(node->type) && node->link)
        node = node->link;

    if (!node || node->type != VFS_FILE)
        return;

    mode_t cleared = node->mode & (mode_t) ~(S_ISUID | S_ISGID);

    if (cleared == node->mode)
        return;

    vfs_chmod(node, cleared);
}

static bool _fd_lookup(sched_thread_t* thread, int fd, sched_fd_t** entry_out) {
    if (!thread || !entry_out)
        return false;

    if (fd < 0 || fd >= SCHED_FD_MAX || !thread->fd_used[fd])
        return false;

    *entry_out = &thread->fds[fd];
    return true;
}

static void _pipe_lock(sched_pipe_t* pipe) {
    if (!pipe)
        return;

    while (__sync_lock_test_and_set(&pipe->lock, 1))
        arch_cpu_wait();
}

static void _pipe_unlock(sched_pipe_t* pipe) {
    if (!pipe)
        return;

    __sync_lock_release(&pipe->lock);
}

static ssize_t _pipe_read_internal(sched_pipe_t* pipe, void* buf, size_t len, bool nonblock) {
    if (!pipe || !buf)
        return -EINVAL;
    if (!len)
        return 0;

    u8* out = buf;
    size_t total = 0;

    for (;;) {
        bool eof = false;

        _pipe_lock(pipe);
        if (pipe->size > 0) {
            size_t chunk = len - total;

            if (chunk > pipe->size)
                chunk = pipe->size;

            size_t first = chunk;

            if (first > pipe->capacity - pipe->read_pos)
                first = pipe->capacity - pipe->read_pos;

            memcpy(out + total, pipe->data + pipe->read_pos, first);

            if (chunk > first)
                memcpy(out + total + first, pipe->data, chunk - first);

            pipe->read_pos = (pipe->read_pos + chunk) % pipe->capacity;
            pipe->size -= chunk;
            total += chunk;
        }

        eof = !pipe->writers;
        _pipe_unlock(pipe);

        if (total > 0) {
            if (pipe->write_wait_queue)
                sched_wake_one(pipe->write_wait_queue);

            return (ssize_t)total;
        }

        if (eof)
            return 0;

        if (nonblock)
            return -EAGAIN;

        sched_thread_t* current = sched_current();

        if (current && sched_signal_has_pending(current))
            return -EINTR;

        if (!sched_is_running())
            continue;

        if (pipe->read_wait_queue)
            sched_block(pipe->read_wait_queue);
    }
}

static ssize_t _pipe_write_internal(sched_pipe_t* pipe, const void* buf, size_t len, bool nonblock) {
    if (!pipe || !buf)
        return -EINVAL;
    if (!len)
        return 0;

    const u8* in = buf;
    size_t total = 0;

    for (;;) {
        bool no_readers = false;

        _pipe_lock(pipe);

        if (!pipe->readers) {
            _pipe_unlock(pipe);
            return total > 0 ? (ssize_t)total : -EPIPE;
        }

        size_t free_space = pipe->capacity - pipe->size;
        if (free_space > 0) {
            size_t chunk = len - total;

            if (chunk > free_space)
                chunk = free_space;

            size_t first = chunk;
            if (first > pipe->capacity - pipe->write_pos)
                first = pipe->capacity - pipe->write_pos;

            memcpy(pipe->data + pipe->write_pos, in + total, first);

            if (chunk > first)
                memcpy(pipe->data, in + total + first, chunk - first);

            pipe->write_pos = (pipe->write_pos + chunk) % pipe->capacity;
            pipe->size += chunk;
            total += chunk;
        }

        no_readers = !pipe->readers;
        _pipe_unlock(pipe);

        if (total > 0) {
            if (pipe->read_wait_queue)
                sched_wake_one(pipe->read_wait_queue);

            if (total == len)
                return (ssize_t)total;
        }

        if (no_readers)
            return total > 0 ? (ssize_t)total : -EPIPE;

        if (nonblock)
            return total > 0 ? (ssize_t)total : -EAGAIN;

        sched_thread_t* current = sched_current();

        if (current && sched_signal_has_pending(current))
            return total > 0 ? (ssize_t)total : -EINTR;

        if (!sched_is_running())
            continue;

        if (pipe->write_wait_queue)
            sched_block(pipe->write_wait_queue);
    }
}

static bool
_split_parent_path(const char* path, char* parent, size_t parent_len, char* base, size_t base_len) {
    if (!path || !parent || !base || parent_len < 2 || !base_len)
        return false;

    const char* slash = strrchr(path, '/');
    if (!slash)
        return false;

    const char* name = slash + 1;
    if (!name[0])
        return false;

    size_t name_len = strnlen(name, base_len);
    if (name_len >= base_len)
        return false;

    memcpy(base, name, name_len);
    base[name_len] = '\0';

    if (slash == path) {
        parent[0] = '/';
        parent[1] = '\0';
        return true;
    }

    size_t dir_len = (size_t)(slash - path);
    if (dir_len + 1 > parent_len)
        return false;

    memcpy(parent, path, dir_len);
    parent[dir_len] = '\0';
    return true;
}

static bool _region_overlaps(const sched_user_region_t* region, uintptr_t start, uintptr_t end) {
    if (!region || start >= end)
        return false;

    uintptr_t region_start = region->vaddr;
    uintptr_t _region_end = region->vaddr + region->pages * PAGE_4KIB;

    return start < _region_end && end > region_start;
}

static uintptr_t _pick_mmap_base(sched_thread_t* thread, size_t size) {
    if (!thread)
        return 0;

    uintptr_t base = 0x00400000;
    uintptr_t stack_base = thread->user_stack_base;

    if (!stack_base)
        stack_base = (uintptr_t)arch_user_stack_top();

    for (sched_user_region_t* region = thread->regions; region; region = region->next) {
        if (region->vaddr == thread->user_stack_base &&
            region->pages == thread->user_stack_size / PAGE_4KIB) {
            continue;
        }

        uintptr_t end = region->vaddr + region->pages * PAGE_4KIB;

        if (end > base)
            base = end;
    }

    base = ALIGN(base, PAGE_4KIB);

    uintptr_t addr = base;
    bool advanced = true;

    while (advanced) {
        advanced = false;

        for (sched_user_region_t* region = thread->regions; region; region = region->next) {
            uintptr_t end = addr + size;

            if (!_region_overlaps(region, addr, end))
                continue;

            addr = ALIGN(region->vaddr + region->pages * PAGE_4KIB, PAGE_4KIB);
            advanced = true;

            break;
        }
    }

    if (addr + size > stack_base)
        return 0;

    return addr;
}

static sched_user_region_t* _find_region_exact(
    sched_thread_t* thread,
    uintptr_t addr,
    size_t pages,
    sched_user_region_t** prev_out
) {
    if (!thread)
        return NULL;

    sched_user_region_t* prev = NULL;

    for (sched_user_region_t* region = thread->regions; region; region = region->next) {
        if (region->vaddr == addr && region->pages == pages) {
            if (prev_out)
                *prev_out = prev;

            return region;
        }

        prev = region;
    }

    return NULL;
}

static u64 _mmap_prot_flags(int prot) {
    u64 flags = PT_USER;

    if (prot & PROT_WRITE)
        flags |= PT_WRITE;

    if (arch_supports_nx() && !(prot & PROT_EXEC))
        flags |= PT_NO_EXECUTE;

    return flags;
}

static uintptr_t _region_end(const sched_user_region_t* region) {
    if (!region)
        return 0;

    return region->vaddr + region->pages * PAGE_4KIB;
}

static ssize_t _sys_read(int fd, void* buf, size_t len) {
    if (!buf)
        return -EFAULT;

    if (!len)
        return 0;

    sched_thread_t* thread = sched_current();
    sched_fd_t* entry = NULL;

    if (thread && fd_lookup(thread, fd, &entry)) {
        _sync_thread_tty_from_entry(thread, entry);

        if (entry->kind == SCHED_FD_PIPE_READ) {
            bool nonblock = (entry->flags & O_NONBLOCK) != 0;
            return _pipe_read_internal(entry->pipe, buf, len, nonblock);
        }

        if (entry->kind != SCHED_FD_VFS || !entry->node)
            return -EBADF;

        if (!((entry->flags & O_RDONLY) || (entry->flags & O_RDWR)))
            return -EBADF;

        size_t vfs_flags = 0;

        if (entry->flags & O_NONBLOCK)
            vfs_flags |= VFS_NONBLOCK;

        ssize_t ret = vfs_read(entry->node, buf, entry->offset, len, vfs_flags);

        if (ret == VFS_EOF)
            return 0;

        if (ret > 0)
            entry->offset += (size_t)ret;

        return ret;
    }

    if (fd == STDIN_FILENO) {
        if (thread)
            thread->tty_index = (int)tty_current_screen();

        tty_handle_t handle = {.kind = TTY_HANDLE_CURRENT, .index = 0};

        return tty_read_handle(&handle, buf, len);
    }

    return -EBADF;
}

static ssize_t _sys_pread(int fd, void* buf, size_t len, off_t offset) {
    if (!buf)
        return -EFAULT;

    if (!len)
        return 0;

    if (offset < 0)
        return -EINVAL;

    sched_thread_t* thread = sched_current();
    sched_fd_t* entry = NULL;

    if (!_fd_lookup(thread, fd, &entry))
        return -EBADF;

    _sync_thread_tty_from_entry(thread, entry);

    if (entry->kind != SCHED_FD_VFS || !entry->node)
        return -ESPIPE;

    if (!((entry->flags & O_RDONLY) || (entry->flags & O_RDWR)))
        return -EBADF;

    size_t vfs_flags = 0;

    if (entry->flags & O_NONBLOCK)
        vfs_flags |= VFS_NONBLOCK;

    ssize_t ret = vfs_read(entry->node, buf, (size_t)offset, len, vfs_flags);

    if (ret == VFS_EOF)
        return 0;

    return ret;
}

static ssize_t _sys_write(int fd, const void* buf, size_t len) {
    if (!buf)
        return -EFAULT;

    if (!len)
        return 0;

    sched_thread_t* thread = sched_current();
    sched_fd_t* entry = NULL;

    if (thread && fd_lookup(thread, fd, &entry)) {
        _sync_thread_tty_from_entry(thread, entry);

        if (entry->kind == SCHED_FD_PIPE_WRITE) {
            bool nonblock = (entry->flags & O_NONBLOCK) != 0;
            return pipe_write_internal(entry->pipe, buf, len, nonblock);
        }

        if (entry->kind != SCHED_FD_VFS || !entry->node)
            return -EBADF;

        if (!((entry->flags & O_WRONLY) || (entry->flags & O_RDWR)))
            return -EBADF;

        size_t vfs_flags = 0;

        if (entry->flags & O_NONBLOCK)
            vfs_flags |= VFS_NONBLOCK;

        size_t offset = entry->offset;

        if (entry->flags & O_APPEND)
            offset = (size_t)entry->node->size;

        ssize_t ret = vfs_write(entry->node, (void*)buf, offset, len, vfs_flags);

        if (ret > 0) {
            entry->offset = offset + (size_t)ret;
            _maybe_clear_setid(thread, entry->node);
        }

        return ret;
    }

    if (fd == STDOUT_FILENO || fd == STDERR_FILENO || fd == STDIN_FILENO) {
        if (thread)
            thread->tty_index = (int)tty_current_screen();

        tty_handle_t handle = {.kind = TTY_HANDLE_CURRENT, .index = 0};

        return tty_write_handle(&handle, buf, len);
    }

    return -EBADF;
}

static ssize_t _sys_pwrite(int fd, const void* buf, size_t len, off_t offset) {
    if (!buf)
        return -EFAULT;

    if (!len)
        return 0;

    if (offset < 0)
        return -EINVAL;

    sched_thread_t* thread = sched_current();
    sched_fd_t* entry = NULL;

    if (!_fd_lookup(thread, fd, &entry))
        return -EBADF;

    _sync_thread_tty_from_entry(thread, entry);

    if (entry->kind != SCHED_FD_VFS || !entry->node)
        return -ESPIPE;

    if (!((entry->flags & O_WRONLY) || (entry->flags & O_RDWR)))
        return -EBADF;

    size_t vfs_flags = 0;

    if (entry->flags & O_NONBLOCK)
        vfs_flags |= VFS_NONBLOCK;

    ssize_t ret = vfs_write(entry->node, (void*)buf, (size_t)offset, len, vfs_flags);
    if (ret > 0)
        _maybe_clear_setid(thread, entry->node);

    return ret;
}

static ssize_t _sys_ioctl(int fd, u64 request, void* args) {
    sched_thread_t* thread = sched_current();
    sched_fd_t* entry = NULL;

    if (thread && fd_lookup(thread, fd, &entry)) {
        if (entry->kind == SCHED_FD_VFS && entry->node) {
            _sync_thread_tty_from_entry(thread, entry);
            return vfs_ioctl(entry->node, request, args);
        }

        return -ENOTTY;
    }

    if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO)
        return -EBADF;

    if (thread)
        thread->tty_index = (int)tty_current_screen();

    tty_handle_t handle = {.kind = TTY_HANDLE_CURRENT, .index = 0};

    return tty_ioctl_handle(&handle, request, args);
}

static int _sys_open(const char* path, int flags, mode_t mode) {
    if (!path)
        return -EFAULT;

    sched_thread_t* thread = sched_current();

    if (!thread)
        return -EINVAL;

    if (!_open_access_valid(flags))
        return -EINVAL;

    if ((flags & O_TRUNC) && !_open_has_write(flags))
        return -EINVAL;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -ENOENT;

    bool ptmx_open = false;
    size_t ptmx_index = 0;
    int fd_tty_index = TTY_NONE;

    if (!strncmp(resolved, "/dev/", 5)) {
        const char* dev = resolved + 5;

        if (!strcmp(dev, "ptmx")) {
            if (!pty_reserve(&ptmx_index))
                return -EAGAIN;

            char master_path[PATH_MAX];
            snprintf(master_path, sizeof(master_path), "/dev/pty%zu", ptmx_index);
            snprintf(resolved, sizeof(resolved), "%s", master_path);
            ptmx_open = true;
            dev = resolved + 5;
        }

        fd_tty_index = _tty_binding_from_dev_name(dev);
    }

    vfs_node_t* node = vfs_lookup(resolved);
    if (node) {
        if ((flags & O_EXCL) && (flags & O_CREAT))
            return _open_fail(ptmx_open, ptmx_index, -EEXIST);
    } else if (flags & O_CREAT) {
        char parent_path[PATH_MAX];
        char base[PATH_MAX];

        if (!split_parent_path(resolved, parent_path, sizeof(parent_path), base, sizeof(base)))
            return _open_fail(ptmx_open, ptmx_index, -EINVAL);

        vfs_node_t* parent = vfs_lookup(parent_path);

        if (parent && VFS_IS_LINK(parent->type))
            parent = parent->link;

        if (!parent || parent->type != VFS_DIR)
            return _open_fail(ptmx_open, ptmx_index, -ENOTDIR);

        if (!vfs_access(parent, thread->uid, thread->gid, W_OK | X_OK))
            return _open_fail(ptmx_open, ptmx_index, -EACCES);

        mode_t create_mode = apply_umask(mode & 07777, thread->umask);
        node = vfs_create(parent, base, VFS_FILE, create_mode);
        if (!node)
            return _open_fail(ptmx_open, ptmx_index, -EIO);

        if (!vfs_chown(node, thread->uid, thread->gid))
            return _open_fail(ptmx_open, ptmx_index, -EIO);
    }

    if (!node)
        return _open_fail(ptmx_open, ptmx_index, -ENOENT);

    vfs_node_t* resolved_node = node;
    if (VFS_IS_LINK(resolved_node->type) && resolved_node->link)
        resolved_node = resolved_node->link;

    if (!resolved_node)
        return _open_fail(ptmx_open, ptmx_index, -EINVAL);

    int need = 0;

    if (_open_has_read(flags))
        need |= R_OK;
    if (_open_has_write(flags))
        need |= W_OK;

    if (resolved_node->type == VFS_DIR && _open_has_write(flags))
        return _open_fail(ptmx_open, ptmx_index, -EISDIR);

    if (need && !vfs_access(resolved_node, thread->uid, thread->gid, need))
        return _open_fail(ptmx_open, ptmx_index, -EACCES);

    if (flags & O_TRUNC) {
        if (resolved_node->type == VFS_DIR)
            return _open_fail(ptmx_open, ptmx_index, -EISDIR);

        if (resolved_node->type == VFS_FILE) {
            if (vfs_truncate(resolved_node, 0) < 0)
                return open_fail(ptmx_open, ptmx_index, -EIO);

            maybe_clear_setid(thread, resolved_node);
        }
    }

    sched_fd_t fd = {
        .kind = SCHED_FD_VFS,
        .node = node,
        .pipe = NULL,
        .offset = 0,
        .pty_index = ptmx_open ? (int)ptmx_index : -1,
        .tty_index = fd_tty_index,
        .flags = (u32)flags,
    };
    int ret = sched_fd_alloc(thread, &fd, 3);

    if (ret < 0 && ptmx_open)
        pty_unreserve(ptmx_index);

    if (ret >= 0 && fd_tty_index != TTY_NONE)
        thread->tty_index = fd_tty_index;

    return ret;
}

static int _sys_close(int fd) {
    if (fd < 0)
        return -EBADF;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    if (fd >= SCHED_FD_MAX)
        return -EBADF;

    if (!thread->fd_used[fd] && fd < 3)
        return 0;

    return sched_fd_close(thread, fd);
}

static int _sys_pipe(int* fds) {
    if (!fds)
        return -EFAULT;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    sched_pipe_t* pipe = sched_pipe_create(SCHED_PIPE_CAPACITY);
    if (!pipe)
        return -ENOMEM;

    sched_fd_t read_end = {
        .kind = SCHED_FD_PIPE_READ,
        .node = NULL,
        .pipe = pipe,
        .offset = 0,
        .pty_index = -1,
        .tty_index = TTY_NONE,
        .flags = O_RDONLY,
    };

    sched_fd_t write_end = {
        .kind = SCHED_FD_PIPE_WRITE,
        .node = NULL,
        .pipe = pipe,
        .offset = 0,
        .pty_index = -1,
        .tty_index = TTY_NONE,
        .flags = O_WRONLY,
    };

    int read_fd = sched_fd_alloc(thread, &read_end, 3);
    if (read_fd < 0) {
        sched_pipe_release_reader(pipe);
        return read_fd;
    }

    int write_fd = sched_fd_alloc(thread, &write_end, 3);
    if (write_fd < 0) {
        sched_fd_close(thread, read_fd);
        return write_fd;
    }

    fds[0] = read_fd;
    fds[1] = write_fd;

    return 0;
}

static int _sys_dup(int oldfd, int newfd) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    return sched_fd_dup(thread, oldfd, newfd);
}

static int _sys_mkdir(const char* path, mode_t mode) {
    if (!path)
        return -EFAULT;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -ENOENT;

    if (vfs_lookup(resolved))
        return -EEXIST;

    char parent_path[PATH_MAX];
    char base[PATH_MAX];

    if (!_split_parent_path(resolved, parent_path, sizeof(parent_path), base, sizeof(base)))
        return -EINVAL;

    vfs_node_t* parent = vfs_lookup(parent_path);
    if (parent && VFS_IS_LINK(parent->type))
        parent = parent->link;

    if (!parent || parent->type != VFS_DIR)
        return -ENOTDIR;

    if (!vfs_access(parent, thread->uid, thread->gid, W_OK | X_OK))
        return -EACCES;

    vfs_node_t* node = vfs_create(parent, base, VFS_DIR, _apply_umask(mode, thread->umask));
    if (!node)
        return -EIO;

    if (!vfs_chown(node, thread->uid, thread->gid))
        return -EIO;

    return 0;
}

static mode_t _sys_umask(mode_t mask) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return (mode_t)-EINVAL;

    mode_t old = thread->umask & 0777;
    thread->umask = mask & 0777;

    return old;
}

static int _sys_rmdir(const char* path) {
    if (!path)
        return -EFAULT;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    char resolved[PATH_MAX];

    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -ENOENT;

    if (!strcmp(resolved, "/"))
        return -EBUSY;

    vfs_node_t* node = vfs_lookup(resolved);
    if (!node)
        return -ENOENT;

    if (VFS_IS_LINK(node->type) && node->link)
        return -ENOTDIR;

    if (node->type != VFS_DIR)
        return -ENOTDIR;

    if (!node->tree_entry)
        return -EIO;

    if (node->tree_entry->children && node->tree_entry->children->length)
        return -ENOTEMPTY;

    char parent_path[PATH_MAX];
    char base[PATH_MAX];

    if (!_split_parent_path(resolved, parent_path, sizeof(parent_path), base, sizeof(base)))
        return -EINVAL;

    vfs_node_t* parent = vfs_lookup(parent_path);

    if (parent && VFS_IS_LINK(parent->type))
        parent = parent->link;

    if (!parent || parent->type != VFS_DIR)
        return -ENOTDIR;

    if (!vfs_access(parent, thread->uid, thread->gid, W_OK | X_OK))
        return -EACCES;

    int sticky = _enforce_sticky_delete(thread, parent, node);
    if (sticky < 0)
        return sticky;

    return vfs_rmdir(resolved) ? 0 : -EIO;
}

static int _sys_chdir(const char* path) {
    if (!path)
        return -EFAULT;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    char resolved[PATH_MAX];

    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -ENOENT;

    vfs_node_t* node = vfs_lookup(resolved);
    if (!node)
        return -ENOENT;

    if (VFS_IS_LINK(node->type) && node->link)
        node = node->link;

    if (!node || node->type != VFS_DIR)
        return -ENOTDIR;

    strncpy(thread->cwd, resolved, sizeof(thread->cwd) - 1);
    thread->cwd[sizeof(thread->cwd) - 1] = '\0';

    return 0;
}

static int _sys_getcwd(char* buf, size_t size) {
    if (!buf)
        return -EFAULT;

    if (!size)
        return -EINVAL;

    if (!_valid_user_ptr(buf, size))
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    size_t len = strnlen(thread->cwd, sizeof(thread->cwd));
    if (len + 1 > size)
        return -ERANGE;

    memcpy(buf, thread->cwd, len + 1);
    return 0;
}

static int _sys_access(const char* path, int mode) {
    if (!path)
        return -EFAULT;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -ENOENT;

    vfs_node_t* node = vfs_lookup(resolved);
    if (!node)
        return -ENOENT;

    if (!mode)
        return 0;

    return vfs_access(node, thread->uid, thread->gid, mode) ? 0 : -EACCES;
}

static uintptr_t _sys_mmap(const mmap_args_t* args) {
    if (!args)
        return (uintptr_t)-EFAULT;

    sched_thread_t* thread = sched_current();
    if (!thread || !thread->vm_space)
        return (uintptr_t)-EINVAL;

    size_t size = args->len;
    if (!size)
        return (uintptr_t)-EINVAL;

    size = ALIGN(size, PAGE_4KIB);
    size_t pages = size / PAGE_4KIB;

    if (args->offset < 0)
        return (uintptr_t)-EINVAL;

    if ((args->offset % PAGE_4KIB) != 0)
        return (uintptr_t)-EINVAL;

    int prot = args->prot;
    if (prot == PROT_NONE)
        return (uintptr_t)-EINVAL;

    int map_type = args->flags & (MAP_SHARED | MAP_PRIVATE);
    if (map_type != MAP_SHARED && map_type != MAP_PRIVATE)
        return (uintptr_t)-EINVAL;

    if (map_type == MAP_SHARED)
        return (uintptr_t)-ENOTSUP;

    if ((args->flags & MAP_ANON) && args->fd != -1)
        return (uintptr_t)-EINVAL;

    uintptr_t addr = (uintptr_t)args->addr;
    if (addr && (addr % PAGE_4KIB))
        return (uintptr_t)-EINVAL;

    bool fixed = (args->flags & MAP_FIXED) != 0;
    if (!addr && fixed)
        return (uintptr_t)-EINVAL;

    if (!addr)
        addr = _pick_mmap_base(thread, size);

    if (!addr)
        return (uintptr_t)-ENOMEM;

    uintptr_t end = addr + size;
    if (end < addr)
        return (uintptr_t)-EINVAL;

    uintptr_t stack_top = thread->user_stack_base;
    if (!stack_top)
        stack_top = (uintptr_t)arch_user_stack_top();

    if (end > stack_top)
        return (uintptr_t)-ENOMEM;

    if (fixed) {
        for (sched_user_region_t* region = thread->regions; region; region = region->next) {
            if (_region_overlaps(region, addr, end))
                return (uintptr_t)-ENOMEM;
        }
    } else {
        for (sched_user_region_t* region = thread->regions; region; region = region->next) {
            if (!_region_overlaps(region, addr, end))
                continue;
            addr = _pick_mmap_base(thread, size);
            end = addr + size;
            break;
        }
    }

    if (!addr || end > stack_top)
        return (uintptr_t)-ENOMEM;

    vfs_node_t* file = NULL;

    if (!(args->flags & MAP_ANON)) {
        int fd = args->fd;

        if (fd < 0)
            return (uintptr_t)-EBADF;

        sched_fd_t* entry = NULL;

        if (!_fd_lookup(thread, fd, &entry))
            return (uintptr_t)-EBADF;

        if (entry->kind != SCHED_FD_VFS || !entry->node)
            return (uintptr_t)-EBADF;

        file = entry->node;

        if (!((entry->flags & O_RDONLY) || (entry->flags & O_RDWR)))
            return (uintptr_t)-EACCES;

        int need = R_OK;
        if (map_type == MAP_SHARED && (prot & PROT_WRITE))
            need |= W_OK;

        if (!vfs_access(file, thread->uid, thread->gid, need))
            return (uintptr_t)-EACCES;
    }

    uintptr_t paddr = (uintptr_t)arch_alloc_frames_user(pages);
    if (!paddr)
        return (uintptr_t)-ENOMEM;

    void* root = arch_vm_root(thread->vm_space);
    if (!root) {
        arch_free_frames((void*)paddr, pages);
        return (uintptr_t)-ENOMEM;
    }

    u64 page_flags = _mmap_prot_flags(prot);
    arch_map_region(root, pages, addr, paddr, page_flags);

    if (!sched_add_user_region(thread, addr, paddr, pages, page_flags)) {
        for (size_t i = 0; i < pages; i++) {
            unmap_page((page_t*)root, addr + i * PAGE_4KIB);
            arch_tlb_flush(addr + i * PAGE_4KIB);
        }

        arch_free_frames((void*)paddr, pages);

        return (uintptr_t)-ENOMEM;
    }

    void* dst = arch_phys_map(paddr, pages * PAGE_4KIB);
    if (!dst) {
        sched_user_region_t* prev = NULL;
        sched_user_region_t* region = _find_region_exact(thread, addr, pages, &prev);

        if (region) {
            if (prev)
                prev->next = region->next;
            else
                thread->regions = region->next;

            free(region);
        }

        for (size_t i = 0; i < pages; i++) {
            uintptr_t vaddr = addr + i * PAGE_4KIB;
            unmap_page((page_t*)root, vaddr);
            arch_tlb_flush(vaddr);
        }

        arch_free_frames((void*)paddr, pages);

        return (uintptr_t)-ENOMEM;
    }

    memset(dst, 0, pages * PAGE_4KIB);

    if (file) {
        ssize_t read_len = vfs_read(file, dst, (size_t)args->offset, size, 0);
        if (read_len < 0) {
            arch_phys_unmap(dst, pages * PAGE_4KIB);

            sched_user_region_t* prev = NULL;
            sched_user_region_t* region = _find_region_exact(thread, addr, pages, &prev);

            if (region) {
                if (prev)
                    prev->next = region->next;
                else
                    thread->regions = region->next;

                free(region);
            }

            for (size_t i = 0; i < pages; i++) {
                uintptr_t vaddr = addr + i * PAGE_4KIB;
                unmap_page((page_t*)root, vaddr);
                arch_tlb_flush(vaddr);
            }

            arch_free_frames((void*)paddr, pages);

            return (uintptr_t)-EIO;
        }
    }

    arch_phys_unmap(dst, pages * PAGE_4KIB);

    return addr;
}

static int _sys_munmap(void* addr, size_t len) {
    if (!addr || !len)
        return -EINVAL;

    sched_thread_t* thread = sched_current();
    if (!thread || !thread->vm_space)
        return -EINVAL;

    uintptr_t base = (uintptr_t)addr;
    if (base % PAGE_4KIB)
        return -EINVAL;

    size_t size = ALIGN(len, PAGE_4KIB);
    uintptr_t end = base + size;
    if (end < base)
        return -EINVAL;

    void* root = arch_vm_root(thread->vm_space);
    if (!root)
        return -EINVAL;

    bool unmapped = false;
    sched_user_region_t* prev = NULL;
    sched_user_region_t* region = thread->regions;

    while (region) {
        sched_user_region_t* next = region->next;
        uintptr_t start = region->vaddr;
        uintptr_t finish = _region_end(region);

        uintptr_t overlap_start = base > start ? base : start;
        uintptr_t overlap_end = end < finish ? end : finish;

        if (overlap_start >= overlap_end) {
            prev = region;
            region = next;
            continue;
        }

        size_t before_pages = (overlap_start - start) / PAGE_4KIB;
        size_t overlap_pages = (overlap_end - overlap_start) / PAGE_4KIB;
        size_t after_pages = (finish - overlap_end) / PAGE_4KIB;
        size_t overlap_page_index = before_pages;

        sched_user_region_t* tail = NULL;
        if (before_pages && after_pages) {
            tail = calloc(1, sizeof(*tail));
            if (!tail)
                return -ENOMEM;

            tail->vaddr = overlap_end;
            tail->paddr = region->paddr + (overlap_page_index + overlap_pages) * PAGE_4KIB;
            tail->pages = after_pages;
            tail->flags = region->flags;
            tail->next = next;
        }

        for (size_t i = 0; i < overlap_pages; i++) {
            uintptr_t vaddr = overlap_start + i * PAGE_4KIB;
            unmap_page((page_t*)root, vaddr);
            arch_tlb_flush(vaddr);
        }

        uintptr_t overlap_paddr = region->paddr + overlap_page_index * (uintptr_t)PAGE_4KIB;
        arch_free_frames((void*)overlap_paddr, overlap_pages);
        unmapped = true;

        if (!before_pages && !after_pages) {
            if (prev)
                prev->next = next;
            else
                thread->regions = next;

            free(region);
            region = next;
            continue;
        }

        if (!before_pages) {
            region->vaddr = overlap_end;
            region->paddr += (overlap_page_index + overlap_pages) * PAGE_4KIB;
            region->pages = after_pages;
            prev = region;
            region = next;
            continue;
        }

        region->pages = before_pages;

        if (tail) {
            region->next = tail;
            prev = tail;
            region = tail->next;
            continue;
        }

        prev = region;
        region = next;
    }

    if (!unmapped)
        return -EINVAL;

    return 0;
}

static long _sys_getuid(void) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    return thread->uid;
}

static long _sys_getgid(void) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    return thread->gid;
}

static int _sys_setuid(uid_t uid) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    if (thread->uid != 0 && uid != thread->uid)
        return -EPERM;

    thread->uid = uid;
    return 0;
}

static int _sys_setgid(gid_t gid) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    if (thread->uid != 0 && gid != thread->gid)
        return -EPERM;

    thread->gid = gid;
    return 0;
}

static pid_t _sys_getpgid(pid_t pid) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    if (!pid)
        return thread->pgid;

    sched_thread_t* target = sched_find_thread(pid);
    if (!target)
        return -ESRCH;

    return target->pgid;
}

static int _sys_setpgid(pid_t pid, pid_t pgid) {
    sched_thread_t* thread = sched_current();
    if (!thread || !thread->user_thread)
        return -EINVAL;

    if (pid < 0 || pgid < 0)
        return -EINVAL;

    sched_thread_t* target = thread;

    if (pid) {
        target = sched_find_thread(pid);

        if (!target || !target->user_thread)
            return -ESRCH;

        if (target->pid != thread->pid && !sched_process_is_child(target->pid, thread->pid))
            return -ESRCH;
    }

    if (target->sid != thread->sid)
        return -EPERM;

    if (target->sid == target->pid)
        return -EPERM;

    if (!pgid)
        pgid = target->pid;

    if (pgid != target->pid && !sched_pgrp_in_session(pgid, thread->sid))
        return -EPERM;

    target->pgid = pgid;
    return 0;
}

static pid_t _sys_setsid(void) {
    sched_thread_t* thread = sched_current();
    if (!thread || !thread->user_thread)
        return -EINVAL;

    if (sched_pgrp_exists(thread->pid))
        return -EPERM;

    thread->sid = thread->pid;
    thread->pgid = thread->pid;
    thread->tty_index = TTY_NONE;

    return thread->sid;
}

static pid_t _sys_waitpid(pid_t pid, int* status, int options) {
    return sched_waitpid(pid, status, options);
}

static int _sys_stat_path(const char* path, stat_t* st, bool follow_links) {
    if (!path || !st)
        return -EFAULT;

    if (!_valid_user_ptr(path, 1) || !_valid_user_ptr(st, sizeof(stat_t)))
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -ENOENT;

    vfs_node_t* node = vfs_lookup(resolved);
    if (!node)
        return -ENOENT;

    return vfs_stat_node(node, st, follow_links) ? 0 : -EIO;
}

static int _sys_fstat(int fd, stat_t* st) {
    if (!st)
        return -EFAULT;

    sched_thread_t* thread = sched_current();
    sched_fd_t* entry = NULL;

    if (!_fd_lookup(thread, fd, &entry))
        return -EBADF;

    if (entry->kind == SCHED_FD_PIPE_READ || entry->kind == SCHED_FD_PIPE_WRITE) {
        memset(st, 0, sizeof(*st));

        st->st_mode = S_IFIFO | 0666;
        st->st_nlink = 1;

        return 0;
    }

    if (entry->kind != SCHED_FD_VFS || !entry->node)
        return -EBADF;

    return vfs_stat_node(entry->node, st, true) ? 0 : -EIO;
}

static int _sys_chmod(const char* path, mode_t mode) {
    if (!path)
        return -EFAULT;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -ENOENT;

    vfs_node_t* node = vfs_lookup(resolved);
    if (!node)
        return -ENOENT;

    if (VFS_IS_LINK(node->type) && node->link)
        node = node->link;

    if (!node)
        return -ENOENT;

    mode_t desired = mode & 07777;

    if (thread->uid != 0) {
        if (node->uid != thread->uid)
            return -EPERM;

        if (desired & S_ISUID)
            return -EPERM;

        if ((desired & S_ISGID) && thread->gid != node->gid)
            return -EPERM;

        if ((desired & S_ISVTX) && node->type != VFS_DIR)
            return -EPERM;
    }

    return vfs_chmod(node, desired) ? 0 : -EIO;
}

static int _sys_chown(const char* path, uid_t uid, gid_t gid) {
    if (!path)
        return -EFAULT;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -ENOENT;

    vfs_node_t* node = vfs_lookup(resolved);
    if (!node)
        return -ENOENT;

    if (VFS_IS_LINK(node->type) && node->link)
        node = node->link;

    if (!node)
        return -ENOENT;

    if (thread->uid != 0)
        return -EPERM;

    uid_t old_uid = node->uid;
    gid_t old_gid = node->gid;
    uid_t new_uid = (uid == (uid_t)-1) ? old_uid : uid;
    gid_t new_gid = (gid == (gid_t)-1) ? old_gid : gid;

    if (!vfs_chown(node, new_uid, new_gid))
        return -EIO;

    if (node->type == VFS_FILE && (old_uid != new_uid || old_gid != new_gid))
        vfs_chmod(node, node->mode & (mode_t) ~(S_ISUID | S_ISGID));

    return 0;
}

static int _sys_link(const char* oldpath, const char* newpath) {
    if (!oldpath || !newpath)
        return -EFAULT;

    if (!_valid_user_ptr(oldpath, 1) || !_valid_user_ptr(newpath, 1))
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    char resolved_old[PATH_MAX];
    char resolved_new[PATH_MAX];

    if (!path_resolve(thread->cwd, oldpath, resolved_old, sizeof(resolved_old)))
        return -ENOENT;

    if (!path_resolve(thread->cwd, newpath, resolved_new, sizeof(resolved_new)))
        return -ENOENT;

    char parent_path[PATH_MAX];
    char base[PATH_MAX];

    if (!_split_parent_path(resolved_new, parent_path, sizeof(parent_path), base, sizeof(base)))
        return -EINVAL;

    vfs_node_t* parent = vfs_lookup(parent_path);
    if (parent && VFS_IS_LINK(parent->type))
        parent = parent->link;

    if (!parent || parent->type != VFS_DIR)
        return -ENOTDIR;

    if (!vfs_access(parent, thread->uid, thread->gid, W_OK | X_OK))
        return -EACCES;

    return vfs_link(resolved_old, resolved_new) ? 0 : -EIO;
}

static int _sys_unlink(const char* path) {
    if (!path)
        return -EFAULT;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -ENOENT;

    char parent_path[PATH_MAX];
    char base[PATH_MAX];

    if (!_split_parent_path(resolved, parent_path, sizeof(parent_path), base, sizeof(base)))
        return -EINVAL;

    vfs_node_t* parent = vfs_lookup(parent_path);
    if (parent && VFS_IS_LINK(parent->type))
        parent = parent->link;

    if (!parent || parent->type != VFS_DIR)
        return -ENOTDIR;

    if (!vfs_access(parent, thread->uid, thread->gid, W_OK | X_OK))
        return -EACCES;

    vfs_node_t* target = vfs_lookup(resolved);
    if (!target)
        return -ENOENT;

    int sticky = _enforce_sticky_delete(thread, parent, target);
    if (sticky < 0)
        return sticky;

    return vfs_unlink(resolved) ? 0 : -EIO;
}

static int _sys_rename(const char* oldpath, const char* newpath) {
    if (!oldpath || !newpath)
        return -EFAULT;

    if (!_valid_user_ptr(oldpath, 1) || !_valid_user_ptr(newpath, 1))
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -EINVAL;

    char resolved_old[PATH_MAX];
    char resolved_new[PATH_MAX];

    if (!path_resolve(thread->cwd, oldpath, resolved_old, sizeof(resolved_old)))
        return -ENOENT;

    if (!path_resolve(thread->cwd, newpath, resolved_new, sizeof(resolved_new)))
        return -ENOENT;

    char old_parent_path[PATH_MAX];
    char old_base[PATH_MAX];

    if (!split_parent_path(
            resolved_old, old_parent_path, sizeof(old_parent_path), old_base, sizeof(old_base)
        ))
        return -EINVAL;

    char new_parent_path[PATH_MAX];
    char new_base[PATH_MAX];

    if (!split_parent_path(
            resolved_new, new_parent_path, sizeof(new_parent_path), new_base, sizeof(new_base)
        ))
        return -EINVAL;

    vfs_node_t* old_parent = vfs_lookup(old_parent_path);

    if (old_parent && VFS_IS_LINK(old_parent->type))
        old_parent = old_parent->link;

    vfs_node_t* new_parent = vfs_lookup(new_parent_path);

    if (new_parent && VFS_IS_LINK(new_parent->type))
        new_parent = new_parent->link;

    if (!old_parent || !new_parent || old_parent->type != VFS_DIR || new_parent->type != VFS_DIR)
        return -ENOTDIR;

    if (!vfs_access(old_parent, thread->uid, thread->gid, W_OK | X_OK))
        return -EACCES;

    if (!vfs_access(new_parent, thread->uid, thread->gid, W_OK | X_OK))
        return -EACCES;

    vfs_node_t* old_node = vfs_lookup(resolved_old);
    if (!old_node)
        return -ENOENT;

    int sticky = _enforce_sticky_delete(thread, old_parent, old_node);
    if (sticky < 0)
        return sticky;

    vfs_node_t* new_node = vfs_lookup(resolved_new);
    if (new_node) {
        sticky = _enforce_sticky_delete(thread, new_parent, new_node);
        if (sticky < 0)
            return sticky;
    }

    return vfs_rename(resolved_old, resolved_new) ? 0 : -EIO;
}

static off_t _sys_seek(int fd, off_t offset, int whence) {
    sched_thread_t* thread = sched_current();
    sched_fd_t* entry = NULL;

    if (!_fd_lookup(thread, fd, &entry))
        return -EBADF;

    if (entry->kind != SCHED_FD_VFS || !entry->node)
        return -ESPIPE;

    off_t base = 0;

    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = (off_t)entry->offset;
        break;
    case SEEK_END:
        base = (off_t)entry->node->size;
        break;
    default:
        return -EINVAL;
    }

    off_t next = base + offset;
    if (next < 0)
        return -EINVAL;

    entry->offset = (size_t)next;
    return next;
}

static int _sys_getdents(int fd, dirent_t* out) {
    if (!out)
        return -EFAULT;

    sched_thread_t* thread = sched_current();
    sched_fd_t* entry = NULL;

    if (!_fd_lookup(thread, fd, &entry))
        return -EBADF;

    if (entry->kind != SCHED_FD_VFS)
        return -ENOTDIR;

    vfs_node_t* node = entry->node;

    if (node && VFS_IS_LINK(node->type) && node->link)
        node = node->link;

    if (!node || node->type != VFS_DIR || !node->tree_entry)
        return -ENOTDIR;

    size_t index = entry->offset;
    size_t current = 0;

    ll_foreach(child, node->tree_entry->children) {
        if (current++ != index)
            continue;

        tree_node_t* tnode = child->data;
        if (!tnode)
            return -EIO;

        vfs_node_t* vnode = tnode->data;
        if (!vnode)
            return -EIO;

        out->d_ino = (u32)vnode->inode;
        out->d_type = (u32)vnode->type;

        memset(out->d_name, 0, sizeof(out->d_name));

        if (vnode->name)
            strncpy(out->d_name, vnode->name, sizeof(out->d_name) - 1);

        entry->offset++;
        return 1;
    }

    return 0;
}

static int _sys_sleep(unsigned int milliseconds) {
    u32 hz = arch_timer_hz();
    if (!hz)
        return -EINVAL;

    u64 ticks = ((u64)milliseconds * (u64)hz + 999ULL) / 1000ULL;
    if (milliseconds && !ticks)
        ticks = 1;

    sched_sleep(ticks);

    return 0;
}

static uintptr_t _sys_signal(int signum, sighandler_t handler, uintptr_t trampoline) {
    sched_thread_t* thread = sched_current();

    if (!thread)
        return (uintptr_t)-EINVAL;

    sighandler_t prev = sched_signal_set_handler(thread, signum, handler, trampoline);

    if (prev == SIG_ERR)
        return (uintptr_t)-EINVAL;

    return (uintptr_t)prev;
}

static u64 _sys_sigreturn(arch_int_state_t* state) {
    sched_thread_t* thread = sched_current();
    if (!thread || !state)
        return (u64)-EINVAL;

    return sched_signal_sigreturn(thread, state) ? 0 : (u64)-EINVAL;
}

static u64 _sys_kill(pid_t pid, int signum) {
    if (pid < 0) {
        int ret = sched_signal_send_pgrp(-pid, signum);
        return ret < 0 ? (u64)-ESRCH : (u64)ret;
    }

    if (!pid) {
        sched_thread_t* thread = sched_current();

        if (!thread || !thread->pgid)
            return (u64)-ESRCH;

        int ret = sched_signal_send_pgrp(thread->pgid, signum);

        return ret < 0 ? (u64)-ESRCH : (u64)ret;
    }

    int ret = sched_signal_send_pid(pid, signum);

    return ret < 0 ? (u64)-ESRCH : (u64)ret;
}

static ssize_t _sys_getprocs(proc_info_t* out, size_t capacity) {
    if (!out || !capacity)
        return -EINVAL;

    return (ssize_t)sched_list_procs(out, capacity);
}

static short _pipe_poll_internal(sched_pipe_t* pipe, bool read_end, short events) {
    if (!pipe)
        return POLLERR;

    size_t size = 0;
    size_t free_space = 0;
    size_t readers = 0;
    size_t writers = 0;

    pipe_lock(pipe);
    size = pipe->size;
    free_space = pipe->capacity - pipe->size;
    readers = pipe->readers;
    writers = pipe->writers;
    pipe_unlock(pipe);

    short revents = 0;

    if (read_end) {
        if ((events & POLLIN) && size)
            revents |= POLLIN;

        if (!writers)
            revents |= POLLHUP;
    } else {
        if ((events & POLLOUT) && readers && free_space)
            revents |= POLLOUT;

        if (!readers)
            revents |= POLLERR | POLLHUP;
    }

    return revents;
}

static short _fd_poll_revents(sched_thread_t* thread, int fd, short events) {
    if (fd < 0)
        return POLLNVAL;

    sched_fd_t* entry = NULL;

    if (thread && fd_lookup(thread, fd, &entry)) {
        if (entry->kind == SCHED_FD_PIPE_READ)
            return _pipe_poll_internal(entry->pipe, true, events);

        if (entry->kind == SCHED_FD_PIPE_WRITE)
            return _pipe_poll_internal(entry->pipe, false, events);

        if (entry->kind == SCHED_FD_VFS && entry->node) {
            size_t vfs_flags = 0;

            if (entry->flags & O_NONBLOCK)
                vfs_flags |= VFS_NONBLOCK;

            short revents = vfs_poll(entry->node, events, vfs_flags);

            if (revents < 0)
                return POLLERR;

            return revents;
        }

        return POLLNVAL;
    }

    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        tty_handle_t handle = {.kind = TTY_HANDLE_CURRENT, .index = 0};
        return tty_poll_handle(&handle, events, 0);
    }

    return POLLNVAL;
}

static int _sys_poll(struct pollfd* fds, nfds_t nfds, int timeout_ms) {
    if (!fds && nfds)
        return -EFAULT;

    if (timeout_ms < -1)
        return -EINVAL;

    if (nfds > 1024)
        return -EINVAL;

    sched_thread_t* thread = sched_current();

    bool finite_timeout = timeout_ms >= 0;
    u64 deadline = 0;

    if (finite_timeout && timeout_ms > 0) {
        u32 hz = arch_timer_hz();
        if (!hz)
            return -EINVAL;

        u64 ticks = ((u64)timeout_ms * (u64)hz + 999ULL) / 1000ULL;
        if (!ticks)
            ticks = 1;

        deadline = arch_timer_ticks() + ticks;
    }

    for (;;) {
        int ready = 0;

        for (nfds_t i = 0; i < nfds; i++) {
            struct pollfd* pfd = &fds[i];
            pfd->revents = 0;

            if (pfd->fd < 0)
                continue;

            short revents = _fd_poll_revents(thread, pfd->fd, pfd->events);
            pfd->revents = revents;

            if (revents)
                ready++;
        }

        if (ready)
            return ready;

        if (finite_timeout && !timeout_ms)
            return 0;

        if (thread && sched_signal_has_pending(thread))
            return -EINTR;

        if (finite_timeout && timeout_ms > 0 && arch_timer_ticks() >= deadline)
            return 0;

        if (!sched_is_running()) {
            arch_cpu_wait();
            continue;
        }

        sched_sleep(1);
    }
}

static u64 _dispatch(arch_int_state_t* state) {
    u64 num = (u64)arch_syscall_num(state);

    switch (num) {
    case SYS_EXIT: {
        int code = (int)arch_syscall_arg1(state);
        sched_thread_t* thread = sched_current();

        if (thread)
            thread->exit_code = code;

        sched_exit();

        return 0;
    }
    case SYS_READ:
        return (u64)_read(
            (int)arch_syscall_arg1(state),
            (void*)arch_syscall_arg2(state),
            (size_t)arch_syscall_arg3(state)
        );
    case SYS_WRITE:
        return (u64)_write(
            (int)arch_syscall_arg1(state),
            (void*)arch_syscall_arg2(state),
            (size_t)arch_syscall_arg3(state)
        );
    case SYS_OPEN:
        return (u64)_sys_open(
            (const char*)arch_syscall_arg1(state),
            (int)arch_syscall_arg2(state),
            (mode_t)arch_syscall_arg3(state)
        );
    case SYS_CLOSE:
        return (u64)_sys_close((int)arch_syscall_arg1(state));
    case SYS_PIPE:
        return (u64)sys_pipe((int*)arch_syscall_arg1(state));
    case SYS_DUP:
        return (u64)_sys_dup((int)arch_syscall_arg1(state), (int)arch_syscall_arg2(state));
    case SYS_PREAD:
        return (u64)_sys_pread(
            (int)arch_syscall_arg1(state),
            (void*)arch_syscall_arg2(state),
            (size_t)arch_syscall_arg3(state),
            (off_t)arch_syscall_arg4(state)
        );
    case SYS_PWRITE:
        return (u64)_sys_pwrite(
            (int)arch_syscall_arg1(state),
            (void*)arch_syscall_arg2(state),
            (size_t)arch_syscall_arg3(state),
            (off_t)arch_syscall_arg4(state)
        );
    case SYS_SEEK:
        return (u64)_sys_seek(
            (int)arch_syscall_arg1(state),
            (off_t)arch_syscall_arg2(state),
            (int)arch_syscall_arg3(state)
        );
    case SYS_MMAP:
        return (u64)_sys_mmap((const mmap_args_t*)arch_syscall_arg1(state));
    case SYS_MUNMAP:
        return (u64)_sys_munmap((void*)arch_syscall_arg1(state), (size_t)arch_syscall_arg2(state));
    case SYS_IOCTL:
        return (u64)_sys_ioctl(
            (int)arch_syscall_arg1(state),
            (u64)arch_syscall_arg2(state),
            (void*)arch_syscall_arg3(state)
        );
    case SYS_GETDENTS:
        return (u64)_sys_getdents((int)arch_syscall_arg1(state), (dirent_t*)arch_syscall_arg2(state));
    case SYS_CHDIR:
        return (u64)_sys_chdir((const char*)arch_syscall_arg1(state));
    case SYS_GETCWD:
        return (u64)_sys_getcwd((char*)arch_syscall_arg1(state), (size_t)arch_syscall_arg2(state));
    case SYS_MKDIR:
        return (u64)_sys_mkdir(
            (const char*)arch_syscall_arg1(state), (mode_t)arch_syscall_arg2(state)
        );
    case SYS_UMASK:
        return (u64)sys_umask((mode_t)arch_syscall_arg1(state));
    case SYS_POLL:
        return (u64)_sys_poll(
            (struct pollfd*)arch_syscall_arg1(state),
            (nfds_t)arch_syscall_arg2(state),
            (int)arch_syscall_arg3(state)
        );
    case SYS_RMDIR:
        return (u64)_sys_rmdir((const char*)arch_syscall_arg1(state));
    case SYS_ACCESS:
        return (u64)sys_access((const char*)arch_syscall_arg1(state), (int)arch_syscall_arg2(state));
    case SYS_STAT:
        return (u64)_sys_stat_path(
            (const char*)arch_syscall_arg1(state), (stat_t*)arch_syscall_arg2(state), true
        );
    case SYS_LSTAT:
        return (u64)_sys_stat_path(
            (const char*)arch_syscall_arg1(state), (stat_t*)arch_syscall_arg2(state), false
        );
    case SYS_FSTAT:
        return (u64)_sys_fstat((int)arch_syscall_arg1(state), (stat_t*)arch_syscall_arg2(state));
    case SYS_CHMOD:
        return (u64)_sys_chmod(
            (const char*)arch_syscall_arg1(state), (mode_t)arch_syscall_arg2(state)
        );
    case SYS_CHOWN:
        return (u64)_sys_chown(
            (const char*)arch_syscall_arg1(state),
            (uid_t)arch_syscall_arg2(state),
            (gid_t)arch_syscall_arg3(state)
        );
    case SYS_LINK:
        return (u64)_sys_link(
            (const char*)arch_syscall_arg1(state), (const char*)arch_syscall_arg2(state)
        );
    case SYS_UNLINK:
        return (u64)_sys_unlink((const char*)arch_syscall_arg1(state));
    case SYS_RENAME:
        return (u64)_sys_rename(
            (const char*)arch_syscall_arg1(state), (const char*)arch_syscall_arg2(state)
        );
    case SYS_FORK:
        return (u64)sched_fork(state);
    case SYS_EXECVE: {
        const char* path = (const char*)arch_syscall_arg1(state);
        if (!path || !_valid_user_ptr(path, 1))
            return (u64)-1;
        return (u64)user_exec(sched_current(), path, state);
    }
    case SYS_WAIT: {
        int* status = (int*)arch_syscall_arg2(state);
        if (status && !_valid_user_ptr(status, sizeof(int)))
            return (u64)-1;
        return (u64)sched_wait(
            (pid_t)arch_syscall_arg1(state),
            status
        );
    case SYS_WAIT:
        return (u64)sched_wait((pid_t)arch_syscall_arg1(state), (int*)arch_syscall_arg2(state));
    case SYS_WAITPID:
        return (u64)_sys_waitpid(
            (pid_t)arch_syscall_arg1(state),
            (int*)arch_syscall_arg2(state),
            (int)arch_syscall_arg3(state)
        );
    case SYS_GETPID: {
        sched_thread_t* thread = sched_current();
        return thread ? (u64)thread->pid : (u64)-ESRCH;
    }
    case SYS_GETPPID: {
        sched_thread_t* thread = sched_current();
        return thread ? (u64)thread->ppid : (u64)-ESRCH;
    }
    case SYS_GETUID:
        return (u64)_sys_getuid();
    case SYS_GETGID:
        return (u64)_sys_getgid();
    case SYS_SETUID:
        return (u64)sys_setuid((uid_t)arch_syscall_arg1(state));
    case SYS_SETGID:
        return (u64)sys_setgid((gid_t)arch_syscall_arg1(state));
    case SYS_SETPGID:
        return (u64)sys_setpgid((pid_t)arch_syscall_arg1(state), (pid_t)arch_syscall_arg2(state));
    case SYS_GETPGID:
        return (u64)sys_getpgid((pid_t)arch_syscall_arg1(state));
    case SYS_SETSID:
        return (u64)sys_setsid();
    case SYS_SLEEP:
        return (u64)_sys_sleep((unsigned int)arch_syscall_arg1(state));
    case SYS_SIGNAL:
        return (u64)_sys_signal(
            (int)arch_syscall_arg1(state),
            (sighandler_t)arch_syscall_arg2(state),
            (uintptr_t)arch_syscall_arg3(state)
        );
    case SYS_SIGRETURN:
        return sys_sigreturn(state);
    case SYS_KILL:
        return _sys_kill((pid_t)arch_syscall_arg1(state), (int)arch_syscall_arg2(state));
    case SYS_GETPROCS:
        return (u64)_sys_getprocs(
            (proc_info_t*)arch_syscall_arg1(state), (size_t)arch_syscall_arg2(state)
        );
    default:
        return (u64)-ENOSYS;
    }
}

static void _handler(arch_int_state_t* state) {
    if (!state)
        return;

    u64 num = (u64)arch_syscall_num(state);
    u64 ret = _dispatch(state);

    if (num == SYS_SIGRETURN && !ret)
        return;

    arch_syscall_set_ret(state, (arch_syscall_t)ret);
}

void syscall_init(void) {
    arch_syscall_install(SYSCALL_INT, _handler);
    log_debug("syscall: interface initialized");
}
