#include "syscall.h"

#include <apheleia/syscall.h>
#include <arch/arch.h>
#include <arch/mm.h>
#include <arch/paging.h>
#include <arch/syscall.h>
#include <arch/thread.h>
#include <base/macros.h>
#include <data/list.h>
#include <data/ring.h>
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
#include <sys/lock.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/path.h>
#include <sys/proc.h>
#include <sys/procfs.h>
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

static int _open_access_mode(int flags) {
    return flags & O_ACCMODE;
}

static bool _open_has_read(int flags) {
    int access = _open_access_mode(flags);
    return access == O_RDONLY || access == O_RDWR;
}

static bool _open_has_write(int flags) {
    int access = _open_access_mode(flags);
    return access == O_WRONLY || access == O_RDWR;
}

static bool _open_access_valid(int flags) {
    int access = _open_access_mode(flags);
    return access == O_RDONLY || access == O_WRONLY || access == O_RDWR;
}

static int _open_fail(bool ptmx_open, size_t ptmx_index, int error) {
    if (ptmx_open) {
        pty_unreserve(ptmx_index);
    }

    return error;
}

static bool _parse_index(const char *text, int max, int *out_index) {
    if (!text || !out_index || !text[0] || max <= 0) {
        return false;
    }

    int value = 0;

    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') {
            return false;
        }

        value = value * 10 + (*p - '0');

        if (value >= max) {
            return false;
        }
    }

    *out_index = value;
    return true;
}

static int _tty_binding_from_dev_name(const char *dev_name) {
    if (!dev_name || !dev_name[0]) {
        return TTY_NONE;
    }

    if (!strcmp(dev_name, "tty")) {
        return (int)tty_current_screen();
    }

    if (!strcmp(dev_name, "console")) {
        return TTY_CONSOLE;
    }

    int idx = 0;

    if (!strncmp(dev_name, "tty", 3) && _parse_index(dev_name + 3, (int)TTY_COUNT, &idx)) {
        return (int)TTY_USER_TO_SCREEN(idx);
    }

    if (!strncmp(dev_name, "pts", 3) && _parse_index(dev_name + 3, (int)PTY_COUNT, &idx)) {
        return PROC_TTY_PTS(idx);
    }

    if (!strncmp(dev_name, "pty", 3) && _parse_index(dev_name + 3, (int)PTY_COUNT, &idx)) {
        return PROC_TTY_PTS(idx);
    }

    return TTY_NONE;
}

static int _pty_index_from_dev_name(const char *dev_name) {
    if (!dev_name || !dev_name[0]) {
        return -1;
    }

    int idx = 0;

    if (!strncmp(dev_name, "pts", 3) &&
        _parse_index(dev_name + 3, (int)PTY_COUNT, &idx)) {
        return idx;
    }

    if (!strncmp(dev_name, "pty", 3) &&
        _parse_index(dev_name + 3, (int)PTY_COUNT, &idx)) {
        return idx;
    }

    return -1;
}

static void _sync_thread_tty(sched_thread_t *thread, const sched_fd_t *entry) {
    if (!thread || !entry) {
        return;
    }

    if (entry->tty_index != TTY_NONE) {
        thread->tty_index = entry->tty_index;
    }
}

static int _enforce_sticky(
    const sched_thread_t *thread,
    vfs_node_t *parent,
    vfs_node_t *target
) {
    if (!thread || !parent || !target) {
        return -EINVAL;
    }

    if (!(parent->mode & S_ISVTX)) {
        return 0;
    }

    if (!thread->uid || thread->uid == parent->uid || thread->uid == target->uid) {
        return 0;
    }

    return -EPERM;
}

static void _maybe_clear_setid(sched_thread_t *thread, vfs_node_t *node) {
    if (!thread || !node || !thread->uid) {
        return;
    }

    if (VFS_IS_LINK(node->type) && node->link) {
        node = node->link;
    }

    if (!node || node->type != VFS_FILE) {
        return;
    }

    mode_t cleared = node->mode & (mode_t) ~(S_ISUID | S_ISGID);

    if (cleared == node->mode) {
        return;
    }

    vfs_chmod(node, cleared);
}

static bool _fd_lookup(sched_thread_t *thread, int fd, sched_fd_t **entry_out) {
    if (!thread || !entry_out) {
        return false;
    }

    if (fd < 0 || fd >= SCHED_FD_MAX || !thread->fd_used[fd]) {
        return false;
    }

    *entry_out = &thread->fds[fd];
    return true;
}

static vfs_node_t *_resolve_link_node(vfs_node_t *node) {
    if (node && VFS_IS_LINK(node->type) && node->link) {
        node = node->link;
    }

    return node;
}

static int _resolve_user_path(
    const sched_thread_t *thread,
    const char *path,
    char *out,
    size_t out_len
) {
    if (!path) {
        return -EFAULT;
    }

    if (!thread) {
        return -EINVAL;
    }

    if (!path_resolve(thread->cwd, path, out, out_len)) {
        return -ENOENT;
    }

    int search_err = vfs_check_search(out, thread->uid, thread->gid, true);
    if (search_err < 0) {
        return search_err;
    }

    return 0;
}

static ssize_t
_pipe_read(sched_pipe_t *pipe, void *buf, size_t len, bool nonblock) {
    if (!pipe || !buf) {
        return -EINVAL;
    }
    if (!len) {
        return 0;
    }

    if (!sched_pipe_operation_begin(pipe)) {
        return -EPIPE;
    }

    u8 *out = buf;
    size_t total = 0;
    ssize_t result = 0;

    for (;;) {
        bool eof = false;
        u32 wait_seq = pipe->read_wait_queue
                           ? sched_wait_seq(pipe->read_wait_queue)
                           : 0;

        unsigned long irq_flags = spin_lock_irqsave(&pipe->lock);
        if (ring_io_size(&pipe->ring)) {
            size_t chunk = ring_io_read(&pipe->ring, out + total, len - total);
            total += chunk;
        }

        eof = !pipe->writers;
        spin_unlock_irqrestore(&pipe->lock, irq_flags);

        if (total > 0) {
            if (pipe->write_wait_queue) {
                sched_wake_one(pipe->write_wait_queue);
            }
            result = (ssize_t)total;
            break;
        }

        if (eof) {
            result = 0;
            break;
        }

        if (nonblock) {
            result = -EAGAIN;
            break;
        }

        sched_thread_t *current = sched_current();

        if (current && sched_signal_has_pending(current)) {
            result = -EINTR;
            break;
        }

        if (!sched_is_running()) {
            continue;
        }

        if (pipe->read_wait_queue) {
            sched_wait_result_t wait_result = sched_wait_on_queue(
                pipe->read_wait_queue,
                wait_seq,
                0,
                SCHED_WAIT_INTERRUPTIBLE
            );
            if (wait_result == SCHED_WAIT_INTR) {
                result = -EINTR;
                break;
            }
        }
    }

    sched_pipe_operation_end(pipe);
    return result;
}

static ssize_t
_pipe_write(sched_pipe_t *pipe, const void *buf, size_t len, bool nonblock) {
    if (!pipe || !buf) {
        return -EINVAL;
    }

    if (!len) {
        return 0;
    }

    if (!sched_pipe_operation_begin(pipe)) {
        return -EPIPE;
    }

    const u8 *in = buf;
    size_t total = 0;
    ssize_t result = 0;

    for (;;) {
        bool no_readers = false;
        u32 wait_seq = pipe->write_wait_queue
                           ? sched_wait_seq(pipe->write_wait_queue)
                           : 0;

        unsigned long irq_flags = spin_lock_irqsave(&pipe->lock);

        if (!pipe->readers) {
            spin_unlock_irqrestore(&pipe->lock, irq_flags);
            result = total > 0 ? (ssize_t)total : -EPIPE;
            break;
        }

        if (ring_io_free_space(&pipe->ring) > 0) {
            size_t chunk = ring_io_write(&pipe->ring, in + total, len - total);
            total += chunk;
        }

        no_readers = !pipe->readers;
        spin_unlock_irqrestore(&pipe->lock, irq_flags);

        if (total > 0) {
            if (pipe->read_wait_queue) {
                sched_wake_one(pipe->read_wait_queue);
            }

            if (total == len) {
                result = (ssize_t)total;
                break;
            }
        }

        if (no_readers) {
            result = total > 0 ? (ssize_t)total : -EPIPE;
            break;
        }

        if (nonblock) {
            result = total > 0 ? (ssize_t)total : -EAGAIN;
            break;
        }

        sched_thread_t *current = sched_current();

        if (current && sched_signal_has_pending(current)) {
            result = total > 0 ? (ssize_t)total : -EINTR;
            break;
        }

        if (!sched_is_running()) {
            continue;
        }

        if (pipe->write_wait_queue) {
            sched_wait_result_t wait_result = sched_wait_on_queue(
                pipe->write_wait_queue,
                wait_seq,
                0,
                SCHED_WAIT_INTERRUPTIBLE
            );
            if (wait_result == SCHED_WAIT_INTR) {
                result = total > 0 ? (ssize_t)total : -EINTR;
                break;
            }
        }
    }

    sched_pipe_operation_end(pipe);
    return result;
}

static bool _split_parent(
    const char *path,
    char *parent,
    size_t parent_len,
    char *base,
    size_t base_len
) {
    if (!path || !parent || !base || parent_len < 2 || !base_len) {
        return false;
    }

    const char *slash = strrchr(path, '/');
    if (!slash) {
        return false;
    }

    const char *name = slash + 1;
    if (!name[0]) {
        return false;
    }

    size_t name_len = strnlen(name, base_len);
    if (name_len >= base_len) {
        return false;
    }

    memcpy(base, name, name_len);
    base[name_len] = '\0';

    if (slash == path) {
        parent[0] = '/';
        parent[1] = '\0';
        return true;
    }

    size_t dir_len = (size_t)(slash - path);
    if (dir_len + 1 > parent_len) {
        return false;
    }

    memcpy(parent, path, dir_len);
    parent[dir_len] = '\0';
    return true;
}

static int _resolve_writable_parent(
    const sched_thread_t *thread,
    const char *path,
    char *parent_path,
    size_t parent_path_len,
    char *base,
    size_t base_len,
    vfs_node_t **parent_out
) {
    if (!thread || !path || !parent_path || !base || !base_len || !parent_out) {
        return -EINVAL;
    }

    if (!_split_parent(path, parent_path, parent_path_len, base, base_len)) {
        return -EINVAL;
    }

    int search_err =
        vfs_check_search(parent_path, thread->uid, thread->gid, false);

    if (search_err < 0) {
        return search_err;
    }

    vfs_node_t *parent = _resolve_link_node(vfs_lookup(parent_path));

    if (!parent || parent->type != VFS_DIR) {
        return -ENOTDIR;
    }

    if (!vfs_access(parent, thread->uid, thread->gid, W_OK | X_OK)) {
        return -EACCES;
    }

    *parent_out = parent;
    return 0;
}

static bool _region_overlaps(
    const sched_user_region_t *region,
    uintptr_t start,
    uintptr_t end
) {
    if (!region || start >= end) {
        return false;
    }

    uintptr_t region_start = region->vaddr;
    uintptr_t _region_end = region->vaddr + region->pages * PAGE_4KIB;

    return start < _region_end && end > region_start;
}

static uintptr_t _pick_mmap_base(sched_thread_t *thread, size_t size) {
    if (!thread) {
        return 0;
    }

    uintptr_t base = 0x00400000;
    uintptr_t stack_base = thread->user_stack_base;

    if (!stack_base) {
        stack_base = (uintptr_t)arch_user_stack_top();
    }

    uintptr_t stack_end = stack_base + thread->user_stack_size;

    for (sched_user_region_t *region = thread->regions; region; region = region->next) {
        uintptr_t region_end = region->vaddr + region->pages * PAGE_4KIB;

        if (stack_base && region->vaddr >= stack_base && region_end <= stack_end) {
            continue;
        }

        if (region_end > base) {
            base = region_end;
        }
    }

    base = ALIGN(base, PAGE_4KIB);

    uintptr_t addr = base;
    bool advanced = true;

    while (advanced) {
        advanced = false;

        for (sched_user_region_t *region = thread->regions; region; region = region->next) {
            uintptr_t end = addr + size;

            if (!_region_overlaps(region, addr, end)) {
                continue;
            }

            addr = ALIGN(region->vaddr + region->pages * PAGE_4KIB, PAGE_4KIB);
            advanced = true;

            break;
        }
    }

    if (addr + size > stack_base) {
        return 0;
    }

    return addr;
}

static sched_user_region_t *_find_region_exact(
    sched_thread_t *thread,
    uintptr_t addr,
    size_t pages,
    sched_user_region_t **prev_out
) {
    if (!thread) {
        return NULL;
    }

    sched_user_region_t *prev = NULL;

    for (sched_user_region_t *region = thread->regions; region; region = region->next) {
        if (region->vaddr == addr && region->pages == pages) {
            if (prev_out) {
                *prev_out = prev;
            }

            return region;
        }

        prev = region;
    }

    return NULL;
}

static u64 _mmap_prot_flags(int prot) {
    u64 flags = PT_USER;

    if (prot & PROT_WRITE) {
        flags |= PT_WRITE;
    }

    if (arch_supports_nx() && !(prot & PROT_EXEC)) {
        flags |= PT_NO_EXECUTE;
    }

    return flags;
}

static uintptr_t _region_end(const sched_user_region_t *region) {
    if (!region) {
        return 0;
    }

    return region->vaddr + region->pages * PAGE_4KIB;
}

static size_t _fd_vfs_io_flags(const sched_fd_t *entry) {
    if (!entry) {
        return 0;
    }

    return (entry->flags & O_NONBLOCK) ? VFS_NONBLOCK : 0;
}

static ssize_t _fd_read_dir(
    sched_fd_t *entry,
    void *buf,
    size_t len,
    size_t offset,
    bool advance_offset
) {
    if (!entry || !buf) {
        return -EINVAL;
    }

    vfs_node_t *node = _resolve_link_node(entry->node);
    if (!node || node->type != VFS_DIR || !node->tree_entry) {
        return -ENOTDIR;
    }

    if (offset % sizeof(dirent_t)) {
        return -EINVAL;
    }

    size_t max_entries = len / sizeof(dirent_t);
    if (!max_entries) {
        return 0;
    }

    dirent_t *out = (dirent_t *)buf;
    size_t start_index = offset / sizeof(dirent_t);
    size_t current = 0;
    size_t written = 0;
    unsigned long procfs_irq_flags = 0;
    bool procfs_locked = procfs_dir_lock_if_needed(node, &procfs_irq_flags);
    ssize_t ret = 0;

    ll_foreach(child, node->tree_entry->children) {
        if (current++ < start_index) {
            continue;
        }

        if (written >= max_entries) {
            break;
        }

        tree_node_t *tnode = child->data;
        if (!tnode) {
            ret = -EIO;
            goto out;
        }

        vfs_node_t *vnode = tnode->data;
        if (!vnode) {
            ret = -EIO;
            goto out;
        }

        out[written].d_ino = vnode->inode;
        out[written].d_type = (unsigned char)vnode->type;
        memset(out[written].d_name, 0, sizeof(out[written].d_name));

        if (vnode->name) {
            strncpy(
                out[written].d_name, vnode->name, sizeof(out[written].d_name) - 1
            );
        }

        written++;
    }

    size_t bytes = written * sizeof(dirent_t);
    if (bytes > 0 && advance_offset) {
        entry->offset = offset + bytes;
    }

    ret = (ssize_t)bytes;

out:
    procfs_dir_unlock_if_needed(procfs_locked, procfs_irq_flags);
    return ret;
}

static ssize_t _fd_read_vfs(
    sched_fd_t *entry,
    void *buf,
    size_t len,
    size_t offset,
    bool advance_offset,
    int bad_kind_error
) {
    if (!entry || entry->kind != SCHED_FD_VFS || !entry->node) {
        return bad_kind_error;
    }

    if (!_open_has_read(entry->flags)) {
        return -EBADF;
    }

    vfs_node_t *node = _resolve_link_node(entry->node);
    if (node && node->type == VFS_DIR) {
        return _fd_read_dir(entry, buf, len, offset, advance_offset);
    }

    ssize_t ret =
        vfs_read(entry->node, buf, offset, len, _fd_vfs_io_flags(entry));

    if (ret == VFS_EOF) {
        return 0;
    }

    if (ret > 0 && advance_offset) {
        entry->offset = offset + (size_t)ret;
    }

    return ret;
}

static ssize_t _fd_write_vfs(
    sched_thread_t *thread,
    sched_fd_t *entry,
    const void *buf,
    size_t len,
    size_t offset,
    bool append_mode,
    bool advance_offset,
    int bad_kind_error
) {
    if (!entry || entry->kind != SCHED_FD_VFS || !entry->node) {
        return bad_kind_error;
    }

    if (!_open_has_write(entry->flags)) {
        return -EBADF;
    }

    if (append_mode && (entry->flags & O_APPEND)) {
        offset = (size_t)entry->node->size;
    }

    ssize_t ret = vfs_write(
        entry->node, (void *)buf, offset, len, _fd_vfs_io_flags(entry)
    );

    if (ret > 0) {
        if (advance_offset) {
            entry->offset = offset + (size_t)ret;
        }
        _maybe_clear_setid(thread, entry->node);
    }

    return ret;
}

static ssize_t sys_read(int fd, void *buf, size_t len) {
    if (!buf) {
        return -EFAULT;
    }

    if (!len) {
        return 0;
    }

    sched_thread_t *thread = sched_current();
    sched_fd_t *entry = NULL;

    if (thread && _fd_lookup(thread, fd, &entry)) {
        _sync_thread_tty(thread, entry);

        if (entry->kind == SCHED_FD_PIPE_READ) {
            bool nonblock = (entry->flags & O_NONBLOCK) != 0;
            return _pipe_read(entry->pipe, buf, len, nonblock);
        }

        return _fd_read_vfs(entry, buf, len, entry->offset, true, -EBADF);
    }

    if (fd == STDIN_FILENO) {
        if (thread) {
            thread->tty_index = (int)tty_current_screen();
        }

        tty_handle_t handle = {.kind = TTY_HANDLE_CURRENT, .index = 0};

        return tty_read_handle(&handle, buf, len);
    }

    return -EBADF;
}

static ssize_t sys_pread(int fd, void *buf, size_t len, off_t offset) {
    if (!buf) {
        return -EFAULT;
    }

    if (!len) {
        return 0;
    }

    if (offset < 0) {
        return -EINVAL;
    }

    sched_thread_t *thread = sched_current();
    sched_fd_t *entry = NULL;

    if (!_fd_lookup(thread, fd, &entry)) {
        return -EBADF;
    }

    _sync_thread_tty(thread, entry);

    return _fd_read_vfs(entry, buf, len, (size_t)offset, false, -ESPIPE);
}

static ssize_t sys_write(int fd, const void *buf, size_t len) {
    if (!buf) {
        return -EFAULT;
    }

    if (!len) {
        return 0;
    }

    sched_thread_t *thread = sched_current();
    sched_fd_t *entry = NULL;

    if (thread && _fd_lookup(thread, fd, &entry)) {
        _sync_thread_tty(thread, entry);

        if (entry->kind == SCHED_FD_PIPE_WRITE) {
            bool nonblock = (entry->flags & O_NONBLOCK) != 0;
            return _pipe_write(entry->pipe, buf, len, nonblock);
        }

        return _fd_write_vfs(
            thread, entry, buf, len, entry->offset, true, true, -EBADF
        );
    }

    if (fd == STDOUT_FILENO || fd == STDERR_FILENO || fd == STDIN_FILENO) {
        if (thread) {
            thread->tty_index = (int)tty_current_screen();
        }

        tty_handle_t handle = {.kind = TTY_HANDLE_CURRENT, .index = 0};

        return tty_write_handle(&handle, buf, len);
    }

    return -EBADF;
}

static ssize_t sys_pwrite(int fd, const void *buf, size_t len, off_t offset) {
    if (!buf) {
        return -EFAULT;
    }

    if (!len) {
        return 0;
    }

    if (offset < 0) {
        return -EINVAL;
    }

    sched_thread_t *thread = sched_current();
    sched_fd_t *entry = NULL;

    if (!_fd_lookup(thread, fd, &entry)) {
        return -EBADF;
    }

    _sync_thread_tty(thread, entry);

    return _fd_write_vfs(
        thread, entry, buf, len, (size_t)offset, false, false, -ESPIPE
    );
}

static ssize_t sys_ioctl(int fd, u64 request, void *args) {
    sched_thread_t *thread = sched_current();
    sched_fd_t *entry = NULL;

    if (thread && _fd_lookup(thread, fd, &entry)) {
        if (entry->kind == SCHED_FD_VFS && entry->node) {
            _sync_thread_tty(thread, entry);
            return vfs_ioctl(entry->node, request, args);
        }

        return -ENOTTY;
    }

    if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        return -EBADF;
    }

    if (thread) {
        thread->tty_index = (int)tty_current_screen();
    }

    tty_handle_t handle = {.kind = TTY_HANDLE_CURRENT, .index = 0};

    return tty_ioctl_handle(&handle, request, args);
}

static int sys_open(const char *path, int flags, mode_t mode) {
    if (!path) {
        return -EFAULT;
    }

    sched_thread_t *thread = sched_current();

    if (!thread) {
        return -EINVAL;
    }

    if (!_open_access_valid(flags)) {
        return -EINVAL;
    }

    if ((flags & O_TRUNC) && !_open_has_write(flags)) {
        return -EINVAL;
    }

    char resolved[PATH_MAX];
    int resolve_err =
        _resolve_user_path(thread, path, resolved, sizeof(resolved));

    if (resolve_err < 0) {
        return resolve_err;
    }

    bool ptmx_open = false;
    size_t ptmx_index = 0;
    int fd_tty_index = TTY_NONE;
    int fd_pty_index = -1;

    if (!strncmp(resolved, "/dev/", 5)) {
        const char *dev = resolved + 5;

        if (!strcmp(dev, "ptmx")) {
            if (!pty_reserve(&ptmx_index)) {
                return -EAGAIN;
            }

            char master_path[PATH_MAX];
            snprintf(
                master_path, sizeof(master_path), "/dev/pty%zu", ptmx_index
            );
            snprintf(resolved, sizeof(resolved), "%s", master_path);
            ptmx_open = true;
            dev = resolved + 5;

            int search_err =
                vfs_check_search(resolved, thread->uid, thread->gid, true);

            if (search_err < 0) {
                return _open_fail(ptmx_open, ptmx_index, search_err);
            }
        }

        fd_tty_index = _tty_binding_from_dev_name(dev);
        fd_pty_index = _pty_index_from_dev_name(dev);
    }

    vfs_node_t *node = vfs_lookup(resolved);
    if (node) {
        if ((flags & O_EXCL) && (flags & O_CREAT)) {
            return _open_fail(ptmx_open, ptmx_index, -EEXIST);
        }
    } else if (flags & O_CREAT) {
        char parent_path[PATH_MAX];
        char base[PATH_MAX];
        vfs_node_t *parent = NULL;

        int parent_err = _resolve_writable_parent(
            thread,
            resolved,
            parent_path,
            sizeof(parent_path),
            base,
            sizeof(base),
            &parent
        );

        if (parent_err < 0) {
            return _open_fail(ptmx_open, ptmx_index, parent_err);
        }

        mode_t create_mode = _apply_umask(mode & 07777, thread->umask);
        node = vfs_create(parent, base, VFS_FILE, create_mode);
        if (!node) {
            return _open_fail(ptmx_open, ptmx_index, -EIO);
        }

        if (!vfs_chown(node, thread->uid, thread->gid)) {
            return _open_fail(ptmx_open, ptmx_index, -EIO);
        }
    }

    if (!node) {
        return _open_fail(ptmx_open, ptmx_index, -ENOENT);
    }

    vfs_node_t *resolved_node = node;
    if (VFS_IS_LINK(resolved_node->type) && resolved_node->link) {
        resolved_node = resolved_node->link;
    }

    if (!resolved_node) {
        return _open_fail(ptmx_open, ptmx_index, -EINVAL);
    }

    int need = 0;

    if (_open_has_read(flags)) {
        need |= R_OK;
    }
    if (_open_has_write(flags)) {
        need |= W_OK;
    }

    if (resolved_node->type == VFS_DIR && _open_has_write(flags)) {
        return _open_fail(ptmx_open, ptmx_index, -EISDIR);
    }

    if (need && !vfs_access(resolved_node, thread->uid, thread->gid, need)) {
        return _open_fail(ptmx_open, ptmx_index, -EACCES);
    }

    if (flags & O_TRUNC) {
        if (resolved_node->type == VFS_DIR) {
            return _open_fail(ptmx_open, ptmx_index, -EISDIR);
        }

        if (resolved_node->type == VFS_FILE) {
            if (vfs_truncate(resolved_node, 0) < 0) {
                return _open_fail(ptmx_open, ptmx_index, -EIO);
            }

            _maybe_clear_setid(thread, resolved_node);
        }
    }

    u32 fd_flags = 0;
    if (flags & O_CLOEXEC) {
        fd_flags |= SCHED_FD_FLAG_CLOEXEC;
    }

    u32 runtime_flags =
        (u32)flags & (u32)(O_ACCMODE | O_APPEND | O_NONBLOCK | O_SYNC);

    sched_fd_t fd = {
        .kind = SCHED_FD_VFS,
        .node = node,
        .pipe = NULL,
        .offset = 0,
        .pty_index = fd_pty_index,
        .tty_index = fd_tty_index,
        .flags = runtime_flags,
        .fd_flags = fd_flags,
    };
    int ret = sched_fd_alloc(thread, &fd, 3);

    if (ret < 0 && ptmx_open) {
        pty_unreserve(ptmx_index);
    }

    if (ret >= 0 && fd_tty_index != TTY_NONE) {
        thread->tty_index = fd_tty_index;
    }

    return ret;
}

static int sys_close(int fd) {
    if (fd < 0) {
        return -EBADF;
    }

    sched_thread_t *thread = sched_current();
    if (!thread) {
        return -EINVAL;
    }

    if (fd >= SCHED_FD_MAX) {
        return -EBADF;
    }

    if (!thread->fd_used[fd]) {
        return -EBADF;
    }

    return sched_fd_close(thread, fd);
}

static int sys_pipe(int *fds) {
    if (!fds) {
        return -EFAULT;
    }

    sched_thread_t *thread = sched_current();
    if (!thread) {
        return -EINVAL;
    }

    sched_pipe_t *pipe = sched_pipe_create(SCHED_PIPE_CAPACITY);
    if (!pipe) {
        return -ENOMEM;
    }

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

static int sys_dup(int oldfd, int newfd) {
    sched_thread_t *thread = sched_current();
    if (!thread) {
        return -EINVAL;
    }

    if (oldfd < 0 || oldfd >= SCHED_FD_MAX || !thread->fd_used[oldfd]) {
        return -EBADF;
    }

    if (newfd < 0) {
        sched_fd_t source = thread->fds[oldfd];
        source.fd_flags &= ~SCHED_FD_FLAG_CLOEXEC;
        return sched_fd_alloc(thread, &source, 0);
    }

    if (newfd >= SCHED_FD_MAX) {
        return -EBADF;
    }

    if (newfd == oldfd) {
        return newfd;
    }

    sched_fd_t source = thread->fds[oldfd];
    source.fd_flags &= ~SCHED_FD_FLAG_CLOEXEC;
    return sched_fd_install(thread, newfd, &source);
}

static int sys_fcntl(int fd, int cmd, uintptr_t arg) {
    sched_thread_t *thread = sched_current();
    if (!thread) {
        return -EINVAL;
    }

    sched_fd_t *entry = NULL;

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        if (!_fd_lookup(thread, fd, &entry)) {
            return -EBADF;
        }

        int min_fd = (int)arg;
        if (min_fd < 0 || min_fd >= SCHED_FD_MAX) {
            return -EINVAL;
        }

        sched_fd_t copy = *entry;
        copy.fd_flags &= ~SCHED_FD_FLAG_CLOEXEC;
        if (cmd == F_DUPFD_CLOEXEC) {
            copy.fd_flags |= SCHED_FD_FLAG_CLOEXEC;
        }

        return sched_fd_alloc(thread, &copy, min_fd);
    }

    case F_GETFD:
        if (!_fd_lookup(thread, fd, &entry)) {
            return -EBADF;
        }
        return (entry->fd_flags & SCHED_FD_FLAG_CLOEXEC) ? FD_CLOEXEC : 0;

    case F_SETFD:
        if (!_fd_lookup(thread, fd, &entry)) {
            return -EBADF;
        }
        if ((int)arg & FD_CLOEXEC) {
            entry->fd_flags |= SCHED_FD_FLAG_CLOEXEC;
        } else {
            entry->fd_flags &= ~SCHED_FD_FLAG_CLOEXEC;
        }
        return 0;

    case F_GETFL:
        if (!_fd_lookup(thread, fd, &entry)) {
            return -EBADF;
        }
        return (int)entry->flags;

    case F_SETFL:
        if (!_fd_lookup(thread, fd, &entry)) {
            return -EBADF;
        }
        entry->flags =
            (entry->flags & ~(O_APPEND | O_NONBLOCK | O_SYNC)) |
            ((u32)arg & (O_APPEND | O_NONBLOCK | O_SYNC));
        return 0;

    default:
        return -EINVAL;
    }
}

static int sys_mkdir(const char *path, mode_t mode) {
    sched_thread_t *thread = sched_current();

    char resolved[PATH_MAX];
    int resolve_err =
        _resolve_user_path(thread, path, resolved, sizeof(resolved));

    if (resolve_err < 0) {
        return resolve_err;
    }

    if (vfs_lookup(resolved)) {
        return -EEXIST;
    }

    char parent_path[PATH_MAX];
    char base[PATH_MAX];
    vfs_node_t *parent = NULL;
    int parent_err = _resolve_writable_parent(
        thread,
        resolved,
        parent_path,
        sizeof(parent_path),
        base,
        sizeof(base),
        &parent
    );

    if (parent_err < 0) {
        return parent_err;
    }

    vfs_node_t *node =
        vfs_create(parent, base, VFS_DIR, _apply_umask(mode, thread->umask));

    if (!node) {
        return -EIO;
    }

    if (!vfs_chown(node, thread->uid, thread->gid)) {
        return -EIO;
    }

    return 0;
}

static int sys_rmdir(const char *path) {
    sched_thread_t *thread = sched_current();

    char resolved[PATH_MAX];
    int resolve_err =
        _resolve_user_path(thread, path, resolved, sizeof(resolved));

    if (resolve_err < 0) {
        return resolve_err;
    }

    if (!strcmp(resolved, "/")) {
        return -EBUSY;
    }

    vfs_node_t *node = vfs_lookup(resolved);
    if (!node) {
        return -ENOENT;
    }

    if (VFS_IS_LINK(node->type) && node->link) {
        return -ENOTDIR;
    }

    if (node->type != VFS_DIR) {
        return -ENOTDIR;
    }

    if (!node->tree_entry) {
        return -EIO;
    }

    if (node->tree_entry->children && node->tree_entry->children->length) {
        return -ENOTEMPTY;
    }

    char parent_path[PATH_MAX];
    char base[PATH_MAX];
    vfs_node_t *parent = NULL;
    int parent_err = _resolve_writable_parent(
        thread,
        resolved,
        parent_path,
        sizeof(parent_path),
        base,
        sizeof(base),
        &parent
    );

    if (parent_err < 0) {
        return parent_err;
    }

    int sticky = _enforce_sticky(thread, parent, node);
    if (sticky < 0) {
        return sticky;
    }

    return vfs_rmdir(resolved) ? 0 : -EIO;
}

static int sys_chdir(const char *path) {
    sched_thread_t *thread = sched_current();

    char resolved[PATH_MAX];
    int resolve_err =
        _resolve_user_path(thread, path, resolved, sizeof(resolved));

    if (resolve_err < 0) {
        return resolve_err;
    }

    vfs_node_t *node = _resolve_link_node(vfs_lookup(resolved));
    if (!node) {
        return -ENOENT;
    }

    if (!node || node->type != VFS_DIR) {
        return -ENOTDIR;
    }

    if (!vfs_access(node, thread->uid, thread->gid, X_OK)) {
        return -EACCES;
    }

    strncpy(thread->cwd, resolved, sizeof(thread->cwd) - 1);
    thread->cwd[sizeof(thread->cwd) - 1] = '\0';

    return 0;
}

static int sys_access(const char *path, int mode) {
    sched_thread_t *thread = sched_current();

    char resolved[PATH_MAX];
    int resolve_err =
        _resolve_user_path(thread, path, resolved, sizeof(resolved));

    if (resolve_err < 0) {
        return resolve_err;
    }

    vfs_node_t *node = vfs_lookup(resolved);
    if (!node) {
        return -ENOENT;
    }

    if (!mode) {
        return 0;
    }

    return vfs_access(node, thread->uid, thread->gid, mode) ? 0 : -EACCES;
}

static uintptr_t sys_mmap(const mmap_args_t *args) {
    if (!args) {
        return (uintptr_t)-EFAULT;
    }

    sched_thread_t *thread = sched_current();
    if (!thread || !thread->vm_space) {
        return (uintptr_t)-EINVAL;
    }

    if (!args->len) {
        return (uintptr_t)-EINVAL;
    }

    size_t size = args->len;
    size = ALIGN(size, PAGE_4KIB);
    size_t pages = size / PAGE_4KIB;

    if (args->offset < 0) {
        return (uintptr_t)-EINVAL;
    }

    if ((args->offset % PAGE_4KIB) != 0) {
        return (uintptr_t)-EINVAL;
    }

    int prot = args->prot;
    if (prot == PROT_NONE) {
        return (uintptr_t)-EINVAL;
    }

    int map_type = args->flags & (MAP_SHARED | MAP_PRIVATE);
    if (map_type != MAP_SHARED && map_type != MAP_PRIVATE) {
        return (uintptr_t)-EINVAL;
    }

    if (map_type == MAP_SHARED) {
        return (uintptr_t)-ENOTSUP;
    }

    if ((args->flags & MAP_ANON) && args->fd != -1) {
        return (uintptr_t)-EINVAL;
    }

    uintptr_t addr = (uintptr_t)args->addr;
    if (addr && (addr % PAGE_4KIB)) {
        return (uintptr_t)-EINVAL;
    }

    bool fixed = (args->flags & MAP_FIXED) != 0;
    if (!addr && fixed) {
        return (uintptr_t)-EINVAL;
    }

    if (!addr) {
        addr = _pick_mmap_base(thread, size);
    }

    if (!addr) {
        return (uintptr_t)-ENOMEM;
    }

    uintptr_t end = addr + size;
    if (end < addr) {
        return (uintptr_t)-EINVAL;
    }

    uintptr_t stack_top = thread->user_stack_base;
    if (!stack_top) {
        stack_top = (uintptr_t)arch_user_stack_top();
    }

    if (end > stack_top) {
        return (uintptr_t)-ENOMEM;
    }

    if (fixed) {
        for (sched_user_region_t *region = thread->regions; region; region = region->next) {
            if (_region_overlaps(region, addr, end)) {
                return (uintptr_t)-ENOMEM;
            }
        }
    } else {
        for (sched_user_region_t *region = thread->regions; region; region = region->next) {
            if (!_region_overlaps(region, addr, end)) {
                continue;
            }

            addr = _pick_mmap_base(thread, size);
            end = addr + size;
            break;
        }
    }

    if (!addr || end > stack_top) {
        return (uintptr_t)-ENOMEM;
    }

    vfs_node_t *file = NULL;

    if (!(args->flags & MAP_ANON)) {
        int fd = args->fd;

        if (fd < 0) {
            return (uintptr_t)-EBADF;
        }

        sched_fd_t *entry = NULL;

        if (!_fd_lookup(thread, fd, &entry)) {
            return (uintptr_t)-EBADF;
        }

        if (entry->kind != SCHED_FD_VFS || !entry->node) {
            return (uintptr_t)-EBADF;
        }

        file = entry->node;

        if (!_open_has_read(entry->flags)) {
            return (uintptr_t)-EACCES;
        }

        int need = R_OK;
        if (map_type == MAP_SHARED && (prot & PROT_WRITE)) {
            need |= W_OK;
        }

        if (!vfs_access(file, thread->uid, thread->gid, need)) {
            return (uintptr_t)-EACCES;
        }
    }

    void *root = arch_vm_root(thread->vm_space);
    if (!root) {
        return (uintptr_t)-ENOMEM;
    }

    u64 page_flags = _mmap_prot_flags(prot);

    uintptr_t paddr = (uintptr_t)arch_alloc_frames_user(pages);
    if (!paddr) {
        return (uintptr_t)-ENOMEM;
    }

    arch_map_region(root, pages, addr, paddr, page_flags);

    if (!sched_add_user_region(thread, addr, paddr, pages, page_flags)) {
        for (size_t i = 0; i < pages; i++) {
            unmap_page((page_t *)root, addr + i * PAGE_4KIB);
            arch_tlb_flush(addr + i * PAGE_4KIB);
        }

        arch_free_frames((void *)paddr, pages);

        return (uintptr_t)-ENOMEM;
    }

    void *dst = arch_phys_map(paddr, pages * PAGE_4KIB, 0);
    if (!dst) {
        sched_user_region_t *prev = NULL;
        sched_user_region_t *region =
            _find_region_exact(thread, addr, pages, &prev);

        if (region) {
            if (prev) {
                prev->next = region->next;
            } else {
                thread->regions = region->next;
            }

            free(region);
            sched_user_mem_sub(thread, pages);
        }

        for (size_t i = 0; i < pages; i++) {
            uintptr_t vaddr = addr + i * PAGE_4KIB;
            unmap_page((page_t *)root, vaddr);
            arch_tlb_flush(vaddr);
        }

        arch_free_frames((void *)paddr, pages);

        return (uintptr_t)-ENOMEM;
    }

    memset(dst, 0, pages * PAGE_4KIB);

    if (file) {
        ssize_t read_len = vfs_read(file, dst, (size_t)args->offset, size, 0);
        if (read_len < 0) {
            arch_phys_unmap(dst, pages * PAGE_4KIB);

            sched_user_region_t *prev = NULL;
            sched_user_region_t *region =
                _find_region_exact(thread, addr, pages, &prev);

            if (region) {
                if (prev) {
                    prev->next = region->next;
                } else {
                    thread->regions = region->next;
                }

                free(region);
                sched_user_mem_sub(thread, pages);
            }

            for (size_t i = 0; i < pages; i++) {
                uintptr_t vaddr = addr + i * PAGE_4KIB;
                unmap_page((page_t *)root, vaddr);
                arch_tlb_flush(vaddr);
            }

            arch_free_frames((void *)paddr, pages);

            return (uintptr_t)-EIO;
        }
    }

    arch_phys_unmap(dst, pages * PAGE_4KIB);

    return addr;
}

static int sys_munmap(void *addr, size_t len) {
    if (!addr || !len) {
        return -EINVAL;
    }

    sched_thread_t *thread = sched_current();
    if (!thread || !thread->vm_space) {
        return -EINVAL;
    }

    uintptr_t base = (uintptr_t)addr;
    if (base % PAGE_4KIB) {
        return -EINVAL;
    }

    size_t size = ALIGN(len, PAGE_4KIB);
    uintptr_t end = base + size;
    if (end < base) {
        return -EINVAL;
    }

    void *root = arch_vm_root(thread->vm_space);
    if (!root) {
        return -EINVAL;
    }

    bool unmapped = false;
    sched_user_region_t *prev = NULL;
    sched_user_region_t *region = thread->regions;

    while (region) {
        sched_user_region_t *next = region->next;
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

        sched_user_region_t *tail = NULL;
        if (before_pages && after_pages) {
            tail = calloc(1, sizeof(*tail));
            if (!tail) {
                return -ENOMEM;
            }

            tail->vaddr = overlap_end;
            tail->paddr =
                region->paddr + (overlap_page_index + overlap_pages) * PAGE_4KIB;
            tail->pages = after_pages;
            tail->flags = region->flags;
            tail->next = next;
        }

        for (size_t i = 0; i < overlap_pages; i++) {
            uintptr_t vaddr = overlap_start + i * PAGE_4KIB;

            unmap_page((page_t *)root, vaddr);
            arch_tlb_flush(vaddr);
        }

        uintptr_t overlap_paddr =
            region->paddr + overlap_page_index * (uintptr_t)PAGE_4KIB;

        arch_free_frames((void *)overlap_paddr, overlap_pages);
        sched_user_mem_sub(thread, overlap_pages);

        unmapped = true;

        if (!before_pages && !after_pages) {
            if (prev) {
                prev->next = next;
            } else {
                thread->regions = next;
            }

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

    if (!unmapped) {
        return -EINVAL;
    }

    return 0;
}

static pid_t sys_waitpid(pid_t pid, int *status, int options) {
    return sched_waitpid(pid, status, options);
}

static int _sys_stat_path(const char *path, stat_t *st, bool follow_links) {
    if (!st) {
        return -EFAULT;
    }

    sched_thread_t *thread = sched_current();

    char resolved[PATH_MAX];
    int resolve_err =
        _resolve_user_path(thread, path, resolved, sizeof(resolved));

    if (resolve_err < 0) {
        return resolve_err;
    }

    vfs_node_t *node = vfs_lookup(resolved);
    if (!node) {
        return -ENOENT;
    }

    if (!vfs_stat_node(node, st, follow_links)) {
        return -EIO;
    }

    uid_t owner_uid = 0;
    gid_t owner_gid = 0;
    if (procfs_stat_owner(node, &owner_uid, &owner_gid)) {
        st->st_uid = owner_uid;
        st->st_gid = owner_gid;
    }

    return 0;
}

static int sys_stat(const char *path, stat_t *st) {
    return _sys_stat_path(path, st, true);
}

static int sys_lstat(const char *path, stat_t *st) {
    return _sys_stat_path(path, st, false);
}

static int sys_fstat(int fd, stat_t *st) {
    if (!st) {
        return -EFAULT;
    }

    sched_thread_t *thread = sched_current();
    sched_fd_t *entry = NULL;

    if (!_fd_lookup(thread, fd, &entry)) {
        return -EBADF;
    }

    if (entry->kind == SCHED_FD_PIPE_READ || entry->kind == SCHED_FD_PIPE_WRITE) {
        memset(st, 0, sizeof(*st));

        st->st_mode = S_IFIFO | 0666;
        st->st_nlink = 1;

        return 0;
    }

    if (entry->kind != SCHED_FD_VFS || !entry->node) {
        return -EBADF;
    }

    if (!vfs_stat_node(entry->node, st, true)) {
        return -EIO;
    }

    uid_t owner_uid = 0;
    gid_t owner_gid = 0;
    if (procfs_stat_owner(entry->node, &owner_uid, &owner_gid)) {
        st->st_uid = owner_uid;
        st->st_gid = owner_gid;
    }

    return 0;
}

static int sys_chmod(const char *path, mode_t mode) {
    sched_thread_t *thread = sched_current();

    char resolved[PATH_MAX];
    int resolve_err =
        _resolve_user_path(thread, path, resolved, sizeof(resolved));

    if (resolve_err < 0) {
        return resolve_err;
    }

    vfs_node_t *node = _resolve_link_node(vfs_lookup(resolved));
    if (!node) {
        return -ENOENT;
    }

    if (!node) {
        return -ENOENT;
    }

    mode_t desired = mode & 07777;

    if (thread->uid != 0) {
        if (node->uid != thread->uid) {
            return -EPERM;
        }

        if (desired & S_ISUID) {
            return -EPERM;
        }

        if ((desired & S_ISGID) && !sched_gid_matches_cred(thread->uid, thread->gid, node->gid)) {
            return -EPERM;
        }

        if ((desired & S_ISVTX) && node->type != VFS_DIR) {
            return -EPERM;
        }
    }

    return vfs_chmod(node, desired) ? 0 : -EIO;
}

static int sys_chown(const char *path, uid_t uid, gid_t gid) {
    sched_thread_t *thread = sched_current();

    char resolved[PATH_MAX];
    int resolve_err =
        _resolve_user_path(thread, path, resolved, sizeof(resolved));

    if (resolve_err < 0) {
        return resolve_err;
    }

    vfs_node_t *node = _resolve_link_node(vfs_lookup(resolved));
    if (!node) {
        return -ENOENT;
    }

    if (!node) {
        return -ENOENT;
    }

    if (thread->uid != 0) {
        return -EPERM;
    }

    uid_t old_uid = node->uid;
    gid_t old_gid = node->gid;
    uid_t new_uid = (uid == (uid_t)-1) ? old_uid : uid;
    gid_t new_gid = (gid == (gid_t)-1) ? old_gid : gid;

    if (!vfs_chown(node, new_uid, new_gid)) {
        return -EIO;
    }

    if (node->type == VFS_FILE && (old_uid != new_uid || old_gid != new_gid)) {
        vfs_chmod(node, node->mode & (mode_t) ~(S_ISUID | S_ISGID));
    }

    return 0;
}

static int sys_link(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) {
        return -EFAULT;
    }

    sched_thread_t *thread = sched_current();
    if (!thread) {
        return -EINVAL;
    }

    char resolved_old[PATH_MAX];
    if (!path_resolve(thread->cwd, oldpath, resolved_old, sizeof(resolved_old))) {
        return -ENOENT;
    }

    char resolved_new[PATH_MAX];
    int resolve_err =
        _resolve_user_path(thread, newpath, resolved_new, sizeof(resolved_new));
    if (resolve_err < 0) {
        return resolve_err;
    }

    char parent_path[PATH_MAX];
    char base[PATH_MAX];
    vfs_node_t *parent = NULL;
    int parent_err = _resolve_writable_parent(
        thread,
        resolved_new,
        parent_path,
        sizeof(parent_path),
        base,
        sizeof(base),
        &parent
    );
    if (parent_err < 0) {
        return parent_err;
    }

    return vfs_link(resolved_old, resolved_new) ? 0 : -EIO;
}

static ssize_t sys_readlink(const char *path, char *buf, size_t bufsiz) {
    if (!path || !buf) {
        return -EFAULT;
    }

    if (!bufsiz) {
        return 0;
    }

    sched_thread_t *thread = sched_current();
    if (!thread) {
        return -EINVAL;
    }

    char resolved[PATH_MAX];
    int resolve_err = _resolve_user_path(thread, path, resolved, sizeof(resolved));
    if (resolve_err < 0) {
        return resolve_err;
    }

    vfs_node_t *node = vfs_lookup(resolved);
    if (!node) {
        return -ENOENT;
    }

    if (node->type != VFS_SYMLINK || !node->symlink_target) {
        return -EINVAL;
    }

    size_t len = strlen(node->symlink_target);
    size_t copy_len = len < bufsiz ? len : bufsiz;
    memcpy(buf, node->symlink_target, copy_len);
    return (ssize_t)copy_len;
}

static int sys_unlink(const char *path) {
    sched_thread_t *thread = sched_current();

    char resolved[PATH_MAX];
    int resolve_err =
        _resolve_user_path(thread, path, resolved, sizeof(resolved));

    if (resolve_err < 0) {
        return resolve_err;
    }

    char parent_path[PATH_MAX];
    char base[PATH_MAX];
    vfs_node_t *parent = NULL;
    int parent_err = _resolve_writable_parent(
        thread,
        resolved,
        parent_path,
        sizeof(parent_path),
        base,
        sizeof(base),
        &parent
    );

    if (parent_err < 0) {
        return parent_err;
    }

    vfs_node_t *target = vfs_lookup(resolved);
    if (!target) {
        return -ENOENT;
    }

    int sticky = _enforce_sticky(thread, parent, target);
    if (sticky < 0) {
        return sticky;
    }

    return vfs_unlink(resolved) ? 0 : -EIO;
}

static int sys_rename(const char *oldpath, const char *newpath) {
    sched_thread_t *thread = sched_current();

    char resolved_old[PATH_MAX];
    char resolved_new[PATH_MAX];
    int resolve_err =
        _resolve_user_path(thread, oldpath, resolved_old, sizeof(resolved_old));

    if (resolve_err < 0) {
        return resolve_err;
    }

    resolve_err =
        _resolve_user_path(thread, newpath, resolved_new, sizeof(resolved_new));

    if (resolve_err < 0) {
        return resolve_err;
    }

    char old_parent_path[PATH_MAX];
    char old_base[PATH_MAX];
    vfs_node_t *old_parent = NULL;

    int old_parent_err = _resolve_writable_parent(
        thread,
        resolved_old,
        old_parent_path,
        sizeof(old_parent_path),
        old_base,
        sizeof(old_base),
        &old_parent
    );

    if (old_parent_err < 0) {
        return old_parent_err;
    }

    char new_parent_path[PATH_MAX];
    char new_base[PATH_MAX];
    vfs_node_t *new_parent = NULL;
    int new_parent_err = _resolve_writable_parent(
        thread,
        resolved_new,
        new_parent_path,
        sizeof(new_parent_path),
        new_base,
        sizeof(new_base),
        &new_parent
    );

    if (new_parent_err < 0) {
        return new_parent_err;
    }

    vfs_node_t *old_node = vfs_lookup(resolved_old);
    if (!old_node) {
        return -ENOENT;
    }

    int sticky = _enforce_sticky(thread, old_parent, old_node);
    if (sticky < 0) {
        return sticky;
    }

    vfs_node_t *new_node = vfs_lookup(resolved_new);
    if (new_node) {
        sticky = _enforce_sticky(thread, new_parent, new_node);
        if (sticky < 0) {
            return sticky;
        }
    }

    return vfs_rename(resolved_old, resolved_new) ? 0 : -EIO;
}

static int sys_mount(
    const char *source,
    const char *target,
    const char *filesystemtype,
    u64 flags
) {
    if (!source || !target || !filesystemtype) {
        return -EFAULT;
    }

    sched_thread_t *thread = sched_current();
    if (!thread) {
        return -EINVAL;
    }

    if (thread->uid != 0) {
        return -EPERM;
    }

    if (flags != 0) {
        return -ENOTSUP;
    }

    if (strcmp(filesystemtype, "ext2")) {
        return -ENOTSUP;
    }

    char resolved_source[PATH_MAX];
    char resolved_target[PATH_MAX];

    int resolve_err = _resolve_user_path(
        thread,
        source,
        resolved_source,
        sizeof(resolved_source)
    );
    if (resolve_err < 0) {
        return resolve_err;
    }

    resolve_err = _resolve_user_path(
        thread,
        target,
        resolved_target,
        sizeof(resolved_target)
    );
    if (resolve_err < 0) {
        return resolve_err;
    }

    vfs_node_t *source_node = _resolve_link_node(vfs_lookup(resolved_source));
    if (!source_node) {
        return -ENOENT;
    }

    vfs_node_t *target_node = _resolve_link_node(vfs_lookup(resolved_target));
    if (!target_node) {
        return -ENOENT;
    }

    if (source_node->type != VFS_BLOCKDEV) {
        return -EINVAL;
    }

    if (!source_node->private) {
        return -ENODEV;
    }

    if (target_node->type == VFS_MOUNT) {
        return -EBUSY;
    }

    if (target_node->type != VFS_DIR) {
        return -ENOTDIR;
    }

    if (!disk_mount_partition_node(source_node, target_node, filesystemtype)) {
        return -ENODEV;
    }

    return 0;
}

static int sys_umount(const char *target, u64 flags) {
    if (!target) {
        return -EFAULT;
    }

    sched_thread_t *thread = sched_current();
    if (!thread) {
        return -EINVAL;
    }

    if (thread->uid != 0) {
        return -EPERM;
    }

    if (flags != 0) {
        return -ENOTSUP;
    }

    char resolved_target[PATH_MAX];
    int resolve_err = _resolve_user_path(
        thread,
        target,
        resolved_target,
        sizeof(resolved_target)
    );
    if (resolve_err < 0) {
        return resolve_err;
    }

    if (!strcmp(resolved_target, "/")) {
        return -EBUSY;
    }

    vfs_node_t *target_node = vfs_lookup(resolved_target);
    if (!target_node) {
        return -ENOENT;
    }

    if (target_node->type != VFS_MOUNT) {
        return -EINVAL;
    }

    if (!disk_unmount_node(target_node, true)) {
        return -EBUSY;
    }

    return 0;
}

static off_t sys_seek(int fd, off_t offset, int whence) {
    sched_thread_t *thread = sched_current();
    sched_fd_t *entry = NULL;

    if (!_fd_lookup(thread, fd, &entry)) {
        return -EBADF;
    }

    if (entry->kind != SCHED_FD_VFS || !entry->node) {
        return -ESPIPE;
    }

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
    if (next < 0) {
        return -EINVAL;
    }

    entry->offset = (size_t)next;
    return next;
}

static int sys_sleep(const struct timespec *req, struct timespec *rem) {
    if (!req) {
        return -EFAULT;
    }

    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        return -EINVAL;
    }

    u64 hz = arch_timer_hz();
    if (!hz) {
        return -EINVAL;
    }

    u64 ns = (u64)req->tv_sec * 1000000000ULL + (u64)req->tv_nsec;
    u64 ticks = (ns * hz + 999999999ULL) / 1000000000ULL;
    if (ns && !ticks) {
        ticks = 1;
    }

    sched_sleep(ticks);

    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }

    return 0;
}

static uintptr_t
sys_signal(int signum, sighandler_t handler, uintptr_t trampoline) {
    sched_thread_t *thread = sched_current();

    if (!thread) {
        return (uintptr_t)-EINVAL;
    }

    sighandler_t prev =
        sched_signal_set_handler(thread, signum, handler, trampoline);

    if (prev == SIG_ERR) {
        return (uintptr_t)-EINVAL;
    }

    return (uintptr_t)prev;
}

static u64 sys_sigreturn(arch_int_state_t *state) {
    sched_thread_t *thread = sched_current();
    if (!thread || !state) {
        return (u64)-EINVAL;
    }

    return sched_signal_sigreturn(thread, state) ? 0 : (u64)-EINVAL;
}

static u64 sys_kill(pid_t pid, int signum) {
    sched_thread_t *self = sched_current();
    if (!self) {
        return (u64)-EINVAL;
    }

    if (pid < 0) {
        int ret = sched_signal_send_pgrp(-pid, signum);
        return ret < 0 ? (u64)-ESRCH : (u64)ret;
    }

    if (!pid) {
        if (!self->pgid) {
            return (u64)-ESRCH;
        }

        int ret = sched_signal_send_pgrp(self->pgid, signum);
        return ret < 0 ? (u64)-ESRCH : (u64)ret;
    }

    // Permission check: non-root can only signal processes with the same uid
    sched_thread_t *target = sched_find_thread(pid);
    if (!target) {
        return (u64)-ESRCH;
    }

    if (self->uid != 0 && self->uid != target->uid) {
        sched_thread_put(target);
        return (u64)-EPERM;
    }

    int ret = sched_signal_send_thread(target, signum);
    sched_thread_put(target);
    return ret < 0 ? (u64)-ESRCH : (u64)ret;
}

static short _pipe_poll(sched_pipe_t *pipe, bool read_end, short events) {
    if (!pipe) {
        return POLLERR;
    }

    size_t size = 0;
    size_t free_space = 0;
    size_t readers = 0;
    size_t writers = 0;

    unsigned long irq_flags = spin_lock_irqsave(&pipe->lock);
    size = ring_io_size(&pipe->ring);
    free_space = ring_io_free_space(&pipe->ring);
    readers = pipe->readers;
    writers = pipe->writers;
    spin_unlock_irqrestore(&pipe->lock, irq_flags);

    short revents = 0;

    if (read_end) {
        if ((events & POLLIN) && size) {
            revents |= POLLIN;
        }

        if (!writers) {
            revents |= POLLHUP;
        }
    } else {
        if ((events & POLLOUT) && readers && free_space) {
            revents |= POLLOUT;
        }

        if (!readers) {
            revents |= POLLERR | POLLHUP;
        }
    }

    return revents;
}

static short _fd_poll_revents(sched_thread_t *thread, int fd, short events) {
    if (fd < 0) {
        return POLLNVAL;
    }

    sched_fd_t *entry = NULL;

    if (thread && _fd_lookup(thread, fd, &entry)) {
        if (entry->kind == SCHED_FD_PIPE_READ) {
            return _pipe_poll(entry->pipe, true, events);
        }

        if (entry->kind == SCHED_FD_PIPE_WRITE) {
            return _pipe_poll(entry->pipe, false, events);
        }

        if (entry->kind == SCHED_FD_VFS && entry->node) {
            size_t vfs_flags = 0;

            if (entry->flags & O_NONBLOCK) {
                vfs_flags |= VFS_NONBLOCK;
            }

            short revents = vfs_poll(entry->node, events, vfs_flags);

            if (revents < 0) {
                return POLLERR;
            }

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

static sched_wait_queue_t *
_pipe_poll_wait_queue(sched_pipe_t *pipe, bool read_end, short events) {
    if (!pipe) {
        return NULL;
    }

    if ((events & POLLIN) && (events & ~POLLIN) == 0) {
        return read_end ? pipe->read_wait_queue : NULL;
    }

    if ((events & POLLOUT) && (events & ~POLLOUT) == 0) {
        return read_end ? NULL : pipe->write_wait_queue;
    }

    return NULL;
}

static sched_wait_queue_t *
_fd_poll_wait_queue(sched_thread_t *thread, int fd, short events) {
    if (fd < 0) {
        return NULL;
    }

    sched_fd_t *entry = NULL;

    if (thread && _fd_lookup(thread, fd, &entry)) {
        if (entry->kind == SCHED_FD_PIPE_READ) {
            return _pipe_poll_wait_queue(entry->pipe, true, events);
        }

        if (entry->kind == SCHED_FD_PIPE_WRITE) {
            return _pipe_poll_wait_queue(entry->pipe, false, events);
        }

        if (entry->kind == SCHED_FD_VFS && entry->node) {
            size_t vfs_flags = 0;

            if (entry->flags & O_NONBLOCK) {
                vfs_flags |= VFS_NONBLOCK;
            }

            return vfs_wait_queue(entry->node, events, vfs_flags);
        }

        return NULL;
    }

    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        tty_handle_t handle = {.kind = TTY_HANDLE_CURRENT, .index = 0};
        return tty_wait_queue_handle(&handle, events, 0);
    }

    return NULL;
}

static int sys_poll(struct pollfd *fds, nfds_t nfds, int timeout_ms) {
    if (!fds && nfds) {
        return -EFAULT;
    }

    if (timeout_ms < -1) {
        return -EINVAL;
    }

    if (nfds > 1024) {
        return -EINVAL;
    }

    sched_thread_t *thread = sched_current();

    bool finite_timeout = timeout_ms >= 0;
    u64 deadline = 0;

    if (finite_timeout && timeout_ms > 0) {
        u32 hz = arch_timer_hz();
        if (!hz) {
            return -EINVAL;
        }

        u64 ticks = ((u64)timeout_ms * (u64)hz + 999ULL) / 1000ULL;
        if (!ticks) {
            ticks = 1;
        }

        deadline = arch_timer_ticks() + ticks;
    }

    for (;;) {
        sched_wait_queue_t *wait_queue = NULL;
        u32 wait_seq = 0;

        if (nfds == 1 && fds && fds[0].fd >= 0) {
            wait_queue = _fd_poll_wait_queue(thread, fds[0].fd, fds[0].events);
        }

        if (wait_queue) {
            wait_seq = sched_wait_seq(wait_queue);
        } else if (nfds) {
            wait_seq = sched_poll_wait_seq();
        }

        int ready = 0;

        for (nfds_t i = 0; i < nfds; i++) {
            struct pollfd *pfd = &fds[i];
            pfd->revents = 0;

            if (pfd->fd < 0) {
                continue;
            }

            short revents = _fd_poll_revents(thread, pfd->fd, pfd->events);
            pfd->revents = revents;

            if (revents) {
                ready++;
            }
        }

        if (ready) {
            return ready;
        }

        if (finite_timeout && !timeout_ms) {
            return 0;
        }

        if (thread && sched_signal_has_pending(thread)) {
            return -EINTR;
        }

        if (finite_timeout && timeout_ms > 0 && arch_timer_ticks() >= deadline) {
            return 0;
        }

        if (!sched_is_running()) {
            arch_cpu_wait();
            continue;
        }

        if (!nfds) {
            sched_wait_result_t wait_result = sched_wait_deadline(
                finite_timeout ? deadline : 0,
                SCHED_WAIT_INTERRUPTIBLE
            );
            if (wait_result == SCHED_WAIT_INTR) {
                return -EINTR;
            }
            if (wait_result == SCHED_WAIT_TIMEOUT) {
                return 0;
            }
            continue;
        }

        if (wait_queue) {
            sched_wait_result_t wait_result = sched_wait_on_queue(
                wait_queue,
                wait_seq,
                finite_timeout ? deadline : 0,
                SCHED_WAIT_INTERRUPTIBLE
            );
            if (wait_result == SCHED_WAIT_INTR) {
                return -EINTR;
            }
            if (wait_result == SCHED_WAIT_TIMEOUT) {
                return 0;
            }
            continue;
        }

        if (!finite_timeout) {
            (void)sched_poll_block_if_unchanged(wait_seq);
            continue;
        }

        if (timeout_ms > 0) {
            (void)sched_poll_block_if_unchanged_until(wait_seq, deadline);
            continue;
        }

        return 0;
    }
}

static u64 _syscall_dispatch(arch_int_state_t *state) {
    u64 num = (u64)arch_syscall_num(state);

    switch (num) {
    case SYS_EXIT: {
        int code = (int)arch_syscall_arg1(state);
        sched_thread_t *thread = sched_current();

        if (thread) {
            thread->exit_code = code;
        }

        sched_exit();

        return 0;
    }

    case SYS_READ:
        return (u64)sys_read(
            (int)arch_syscall_arg1(state),
            (void *)arch_syscall_arg2(state),
            (size_t)arch_syscall_arg3(state)
        );
    case SYS_WRITE:
        return (u64)sys_write(
            (int)arch_syscall_arg1(state),
            (void *)arch_syscall_arg2(state),
            (size_t)arch_syscall_arg3(state)
        );
    case SYS_OPEN:
        return (u64)sys_open(
            (const char *)arch_syscall_arg1(state),
            (int)arch_syscall_arg2(state),
            (mode_t)arch_syscall_arg3(state)
        );
    case SYS_CLOSE:
        return (u64)sys_close((int)arch_syscall_arg1(state));
    case SYS_PIPE:
        return (u64)sys_pipe((int *)arch_syscall_arg1(state));
    case SYS_DUP:
        return (u64)sys_dup(
            (int)arch_syscall_arg1(state), (int)arch_syscall_arg2(state)
        );
    case SYS_FCNTL:
        return (u64)sys_fcntl(
            (int)arch_syscall_arg1(state),
            (int)arch_syscall_arg2(state),
            (uintptr_t)arch_syscall_arg3(state)
        );
    case SYS_PREAD:
        return (u64)sys_pread(
            (int)arch_syscall_arg1(state),
            (void *)arch_syscall_arg2(state),
            (size_t)arch_syscall_arg3(state),
            (off_t)arch_syscall_arg4(state)
        );
    case SYS_PWRITE:
        return (u64)sys_pwrite(
            (int)arch_syscall_arg1(state),
            (void *)arch_syscall_arg2(state),
            (size_t)arch_syscall_arg3(state),
            (off_t)arch_syscall_arg4(state)
        );
    case SYS_SEEK:
        return (u64)sys_seek(
            (int)arch_syscall_arg1(state),
            (off_t)arch_syscall_arg2(state),
            (int)arch_syscall_arg3(state)
        );
    case SYS_MMAP:
        return (u64)sys_mmap((const mmap_args_t *)arch_syscall_arg1(state));
    case SYS_MUNMAP:
        return (u64)sys_munmap(
            (void *)arch_syscall_arg1(state), (size_t)arch_syscall_arg2(state)
        );
    case SYS_IOCTL:
        return (u64)sys_ioctl(
            (int)arch_syscall_arg1(state),
            (u64)arch_syscall_arg2(state),
            (void *)arch_syscall_arg3(state)
        );
    case SYS_CHDIR:
        return (u64)sys_chdir((const char *)arch_syscall_arg1(state));
    case SYS_MKDIR:
        return (u64)sys_mkdir(
            (const char *)arch_syscall_arg1(state),
            (mode_t)arch_syscall_arg2(state)
        );
    case SYS_POLL:
        return (u64)sys_poll(
            (struct pollfd *)arch_syscall_arg1(state),
            (nfds_t)arch_syscall_arg2(state),
            (int)arch_syscall_arg3(state)
        );
    case SYS_RMDIR:
        return (u64)sys_rmdir((const char *)arch_syscall_arg1(state));
    case SYS_ACCESS:
        return (u64)sys_access(
            (const char *)arch_syscall_arg1(state),
            (int)arch_syscall_arg2(state)
        );
    case SYS_STAT:
        return (u64)sys_stat(
            (const char *)arch_syscall_arg1(state), (stat_t *)arch_syscall_arg2(state)
        );
    case SYS_LSTAT:
        return (u64)sys_lstat(
            (const char *)arch_syscall_arg1(state), (stat_t *)arch_syscall_arg2(state)
        );
    case SYS_FSTAT:
        return (u64)sys_fstat(
            (int)arch_syscall_arg1(state), (stat_t *)arch_syscall_arg2(state)
        );
    case SYS_CHMOD:
        return (u64)sys_chmod(
            (const char *)arch_syscall_arg1(state),
            (mode_t)arch_syscall_arg2(state)
        );
    case SYS_CHOWN:
        return (u64)sys_chown(
            (const char *)arch_syscall_arg1(state),
            (uid_t)arch_syscall_arg2(state),
            (gid_t)arch_syscall_arg3(state)
        );
    case SYS_LINK:
        return (u64)sys_link(
            (const char *)arch_syscall_arg1(state),
            (const char *)arch_syscall_arg2(state)
        );
    case SYS_READLINK:
        return (u64)sys_readlink(
            (const char *)arch_syscall_arg1(state),
            (char *)arch_syscall_arg2(state),
            (size_t)arch_syscall_arg3(state)
        );
    case SYS_UNLINK:
        return (u64)sys_unlink((const char *)arch_syscall_arg1(state));
    case SYS_RENAME:
        return (u64)sys_rename(
            (const char *)arch_syscall_arg1(state),
            (const char *)arch_syscall_arg2(state)
        );
    case SYS_FORK:
        return (u64)sched_fork(state);
    case SYS_EXECVE:
        return (u64)user_exec(
            sched_current(),
            (const char *)arch_syscall_arg1(state),
            (char *const *)arch_syscall_arg2(state),
            (char *const *)arch_syscall_arg3(state),
            state
        );
    case SYS_WAIT:
        return (u64)sched_wait(
            (pid_t)arch_syscall_arg1(state), (int *)arch_syscall_arg2(state)
        );
    case SYS_WAITPID:
        return (u64)sys_waitpid(
            (pid_t)arch_syscall_arg1(state),
            (int *)arch_syscall_arg2(state),
            (int)arch_syscall_arg3(state)
        );
    case SYS_SLEEP:
        return (u64)sys_sleep(
            (const struct timespec *)arch_syscall_arg1(state),
            (struct timespec *)arch_syscall_arg2(state)
        );
    case SYS_MOUNT:
        return (u64)sys_mount(
            (const char *)arch_syscall_arg1(state),
            (const char *)arch_syscall_arg2(state),
            (const char *)arch_syscall_arg3(state),
            (u64)arch_syscall_arg4(state)
        );
    case SYS_UMOUNT:
        return (u64)sys_umount(
            (const char *)arch_syscall_arg1(state),
            (u64)arch_syscall_arg2(state)
        );
    case SYS_SIGNAL:
        return (u64)sys_signal(
            (int)arch_syscall_arg1(state),
            (sighandler_t)arch_syscall_arg2(state),
            (uintptr_t)arch_syscall_arg3(state)
        );
    case SYS_SIGRETURN:
        return sys_sigreturn(state);
    case SYS_KILL:
        return sys_kill(
            (pid_t)arch_syscall_arg1(state), (int)arch_syscall_arg2(state)
        );
    default:
        return (u64)-ENOSYS;
    }
}

static void _syscall_handler(arch_int_state_t *state) {
    if (!state) {
        return;
    }

    sched_capture_context(state);

    u64 num = (u64)arch_syscall_num(state);
    u64 ret = _syscall_dispatch(state);

    if (num == SYS_SIGRETURN && !ret) {
        return;
    }

    arch_syscall_set_ret(state, (arch_syscall_t)ret);
    sched_signal_deliver_current(state);
}

void syscall_init(void) {
    arch_syscall_install(SYSCALL_INT, _syscall_handler);
    log_debug("syscall interface initialized");
}
