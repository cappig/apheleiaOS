#include "syscall.h"

#include <arch/arch.h>
#include <arch/syscall.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sys/exec.h>
#include <sys/path.h>
#include <sys/stat.h>
#include <sys/tty.h>
#include <unistd.h>

#define SYSCALL_INT 0x80

static bool
_split_parent_path(const char* path, char* parent, size_t parent_len, char* base, size_t base_len);

static ssize_t _sys_read(int fd, void* buf, size_t len) {
    if (!buf || !len)
        return 0;

    if (!_valid_user_ptr(buf, len))
        return -1;

    if (fd != STDIN_FILENO)
        return -1;

    if (!((entry->flags & O_RDONLY) || (entry->flags & O_RDWR)))
        return -1;

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

static ssize_t _sys_pread(int fd, void* buf, size_t len, off_t offset) {
    if (!buf || !len)
        return 0;

    if (fd < 3)
        return -1;

    if (offset < 0)
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread || fd < 0 || fd >= SCHED_FD_MAX || !thread->fd_used[fd])
        return -1;

    sched_fd_t* entry = &thread->fds[fd];
    if (!entry->node)
        return -1;

    if (!((entry->flags & O_RDONLY) || (entry->flags & O_RDWR)))
        return -1;

    size_t vfs_flags = 0;
    if (entry->flags & O_NONBLOCK)
        vfs_flags |= VFS_NONBLOCK;

    ssize_t ret = vfs_read(entry->node, buf, (size_t)offset, len, vfs_flags);
    if (ret == VFS_EOF)
        return 0;
    return ret;
}

static ssize_t _sys_write(int fd, const void* buf, size_t len) {
    if (!buf || !len)
        return 0;

    if (!_valid_user_ptr(buf, len))
        return -1;

    if (fd != STDOUT_FILENO && fd != STDERR_FILENO && fd != STDIN_FILENO)
        return -1;

    if (!((entry->flags & O_WRONLY) || (entry->flags & O_RDWR)))
        return -1;

    size_t vfs_flags = 0;
    if (entry->flags & O_NONBLOCK)
        vfs_flags |= VFS_NONBLOCK;

    size_t offset = entry->offset;
    if (entry->flags & O_APPEND)
        offset = (size_t)entry->node->size;

    ssize_t ret = vfs_write(entry->node, (void*)buf, offset, len, vfs_flags);
    if (ret > 0)
        entry->offset = offset + (size_t)ret;
    return ret;
}

static ssize_t _sys_pwrite(int fd, const void* buf, size_t len, off_t offset) {
    if (!buf || !len)
        return 0;

    if (fd < 3)
        return -1;

    if (offset < 0)
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread || fd < 0 || fd >= SCHED_FD_MAX || !thread->fd_used[fd])
        return -1;

    sched_fd_t* entry = &thread->fds[fd];
    if (!entry->node)
        return -1;

    if (!((entry->flags & O_WRONLY) || (entry->flags & O_RDWR)))
        return -1;

    size_t vfs_flags = 0;
    if (entry->flags & O_NONBLOCK)
        vfs_flags |= VFS_NONBLOCK;

    return vfs_write(entry->node, (void*)buf, (size_t)offset, len, vfs_flags);
}

static ssize_t _sys_ioctl(int fd, u64 request, void* args) {
    if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO)
        return -1;

    if (args && !_valid_user_ptr(args, 1))
        return -1;

    tty_handle_t handle = {.kind = TTY_HANDLE_CURRENT, .index = 0};
    return tty_ioctl_handle(&handle, request, args);
}

static int _sys_open(const char* path, int flags, mode_t mode) {
    if (!path || !_valid_user_ptr(path, 1))
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -1;

    vfs_node_t* node = vfs_lookup(resolved);
    if (node) {
        if ((flags & O_EXCL) && (flags & O_CREAT))
            return -1;
    } else if (flags & O_CREAT) {
        char parent_path[PATH_MAX];
        char base[PATH_MAX];
        if (!_split_parent_path(resolved, parent_path, sizeof(parent_path), base, sizeof(base)))
            return -1;

        vfs_node_t* parent = vfs_lookup(parent_path);
        if (parent && VFS_IS_LINK(parent->type))
            parent = parent->link;

        if (!parent || parent->type != VFS_DIR)
            return -1;

        if (!vfs_access(parent, thread->uid, thread->gid, W_OK | X_OK))
            return -1;

        node = vfs_create(parent, base, VFS_FILE, mode);
        if (!node)
            return -1;

        node->uid = thread->uid;
        node->gid = thread->gid;
    }

    if (!node)
        return -1;

    int need = 0;
    if (flags & O_RDWR)
        need |= R_OK | W_OK;
    else if (flags & O_WRONLY)
        need |= W_OK;
    else if (flags & O_RDONLY)
        need |= R_OK;

    if (need && !vfs_access(node, thread->uid, thread->gid, need))
        return -1;

    for (int fd = 3; fd < SCHED_FD_MAX; fd++) {
        if (!thread->fd_used[fd]) {
            thread->fd_used[fd] = true;
            thread->fds[fd].node = node;
            thread->fds[fd].offset = 0;
            thread->fds[fd].flags = (u32)flags;
            return fd;
        }
    }

    return -1;
}

static bool
_split_parent_path(const char* path, char* parent, size_t parent_len, char* base, size_t base_len) {
    if (!path || !parent || !base || parent_len < 2 || base_len == 0)
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

static int _sys_close(int fd) {
    if (fd < 0)
        return -1;

    if (fd < 3)
        return 0;

    sched_thread_t* thread = sched_current();
    if (!thread || fd >= SCHED_FD_MAX || !thread->fd_used[fd])
        return -1;

    thread->fd_used[fd] = false;
    thread->fds[fd].node = NULL;
    thread->fds[fd].offset = 0;
    thread->fds[fd].flags = 0;
    return 0;
}

static int _sys_mkdir(const char* path, mode_t mode) {
    if (!path)
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -1;

    if (vfs_lookup(resolved))
        return -1;

    char parent_path[PATH_MAX];
    char base[PATH_MAX];
    if (!_split_parent_path(resolved, parent_path, sizeof(parent_path), base, sizeof(base)))
        return -1;

    vfs_node_t* parent = vfs_lookup(parent_path);
    if (parent && VFS_IS_LINK(parent->type))
        parent = parent->link;

    if (!parent || parent->type != VFS_DIR)
        return -1;

    if (!vfs_access(parent, thread->uid, thread->gid, W_OK | X_OK))
        return -1;

    vfs_node_t* node = vfs_create(parent, base, VFS_DIR, mode);
    if (!node)
        return -1;

    node->uid = thread->uid;
    node->gid = thread->gid;
    return 0;
}

static int _sys_chdir(const char* path) {
    if (!path)
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -1;

    vfs_node_t* node = vfs_lookup(resolved);
    if (!node)
        return -1;

    if (VFS_IS_LINK(node->type) && node->link)
        node = node->link;

    if (!node || node->type != VFS_DIR)
        return -1;

    strncpy(thread->cwd, resolved, sizeof(thread->cwd) - 1);
    thread->cwd[sizeof(thread->cwd) - 1] = '\0';
    return 0;
}

static int _sys_getcwd(char* buf, size_t size) {
    if (!buf || !size)
        return -1;

    if (!_valid_user_ptr(buf, size))
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    size_t len = strnlen(thread->cwd, sizeof(thread->cwd));
    if (len + 1 > size)
        return -1;

    memcpy(buf, thread->cwd, len + 1);
    return 0;
}

static int _sys_access(const char* path, int mode) {
    if (!path)
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -1;

    vfs_node_t* node = vfs_lookup(resolved);
    if (!node)
        return -1;

    if (mode == 0)
        return 0;

    return vfs_access(node, thread->uid, thread->gid, mode) ? 0 : -1;
}

static uid_t _sys_getuid(void) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return (uid_t)-1;

    return thread->uid;
}

static gid_t _sys_getgid(void) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return (gid_t)-1;

    return thread->gid;
}

static int _sys_setuid(uid_t uid) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    if (thread->uid != 0 && uid != thread->uid)
        return -1;

    thread->uid = uid;
    return 0;
}

static int _sys_setgid(gid_t gid) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    if (thread->uid != 0 && gid != thread->gid)
        return -1;

    thread->gid = gid;
    return 0;
}

static pid_t _sys_getpgid(pid_t pid) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    if (pid == 0)
        return thread->pgid;

    sched_thread_t* target = sched_find_thread(pid);
    if (!target)
        return -1;

    return target->pgid;
}

static int _sys_setpgid(pid_t pid, pid_t pgid) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    sched_thread_t* target = thread;
    if (pid != 0) {
        target = sched_find_thread(pid);
        if (!target)
            return -1;
    }

    if (target->sid != thread->sid)
        return -1;

    if (pgid == 0)
        pgid = target->pid;

    target->pgid = pgid;
    return 0;
}

static pid_t _sys_setsid(void) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    if (thread->pid == thread->pgid)
        return -1;

    thread->sid = thread->pid;
    thread->pgid = thread->pid;
    return thread->sid;
}

static int _sys_stat_path(const char* path, stat_t* st, bool follow_links) {
    if (!path || !st)
        return -1;

    if (!_valid_user_ptr(path, 1) || !_valid_user_ptr(st, sizeof(stat_t)))
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -1;

    vfs_node_t* node = vfs_lookup(resolved);
    if (!node)
        return -1;

    return vfs_stat_node(node, st, follow_links) ? 0 : -1;
}

static int _sys_fstat(int fd, stat_t* st) {
    if (!st || !_valid_user_ptr(st, sizeof(stat_t)))
        return -1;

    if (fd < 3)
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread || fd < 0 || fd >= SCHED_FD_MAX || !thread->fd_used[fd])
        return -1;

    sched_fd_t* entry = &thread->fds[fd];
    if (!entry->node)
        return -1;

    return vfs_stat_node(entry->node, st, true) ? 0 : -1;
}

static int _sys_chmod(const char* path, mode_t mode) {
    if (!path || !_valid_user_ptr(path, 1))
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -1;

    vfs_node_t* node = vfs_lookup(resolved);
    if (!node)
        return -1;

    if (thread->uid != 0 && node->uid != thread->uid)
        return -1;

    return vfs_chmod(node, mode) ? 0 : -1;
}

static int _sys_chown(const char* path, uid_t uid, gid_t gid) {
    if (!path || !_valid_user_ptr(path, 1))
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -1;

    vfs_node_t* node = vfs_lookup(resolved);
    if (!node)
        return -1;

    if (thread->uid != 0)
        return -1;

    return vfs_chown(node, uid, gid) ? 0 : -1;
}

static int _sys_link(const char* oldpath, const char* newpath) {
    if (!oldpath || !newpath)
        return -1;

    if (!_valid_user_ptr(oldpath, 1) || !_valid_user_ptr(newpath, 1))
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    char resolved_old[PATH_MAX];
    char resolved_new[PATH_MAX];

    if (!path_resolve(thread->cwd, oldpath, resolved_old, sizeof(resolved_old)))
        return -1;

    if (!path_resolve(thread->cwd, newpath, resolved_new, sizeof(resolved_new)))
        return -1;

    char parent_path[PATH_MAX];
    char base[PATH_MAX];
    if (!_split_parent_path(resolved_new, parent_path, sizeof(parent_path), base, sizeof(base)))
        return -1;

    vfs_node_t* parent = vfs_lookup(parent_path);
    if (parent && VFS_IS_LINK(parent->type))
        parent = parent->link;

    if (!parent || parent->type != VFS_DIR)
        return -1;

    if (!vfs_access(parent, thread->uid, thread->gid, W_OK | X_OK))
        return -1;

    return vfs_link(resolved_old, resolved_new) ? 0 : -1;
}

static int _sys_unlink(const char* path) {
    if (!path || !_valid_user_ptr(path, 1))
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    char resolved[PATH_MAX];
    if (!path_resolve(thread->cwd, path, resolved, sizeof(resolved)))
        return -1;

    char parent_path[PATH_MAX];
    char base[PATH_MAX];
    if (!_split_parent_path(resolved, parent_path, sizeof(parent_path), base, sizeof(base)))
        return -1;

    vfs_node_t* parent = vfs_lookup(parent_path);
    if (parent && VFS_IS_LINK(parent->type))
        parent = parent->link;

    if (!parent || parent->type != VFS_DIR)
        return -1;

    if (!vfs_access(parent, thread->uid, thread->gid, W_OK | X_OK))
        return -1;

    return vfs_unlink(resolved) ? 0 : -1;
}

static int _sys_rename(const char* oldpath, const char* newpath) {
    if (!oldpath || !newpath)
        return -1;

    if (!_valid_user_ptr(oldpath, 1) || !_valid_user_ptr(newpath, 1))
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread)
        return -1;

    char resolved_old[PATH_MAX];
    char resolved_new[PATH_MAX];

    if (!path_resolve(thread->cwd, oldpath, resolved_old, sizeof(resolved_old)))
        return -1;

    if (!path_resolve(thread->cwd, newpath, resolved_new, sizeof(resolved_new)))
        return -1;

    char old_parent_path[PATH_MAX];
    char old_base[PATH_MAX];
    if (!_split_parent_path(
            resolved_old, old_parent_path, sizeof(old_parent_path), old_base, sizeof(old_base)
        ))
        return -1;

    char new_parent_path[PATH_MAX];
    char new_base[PATH_MAX];
    if (!_split_parent_path(
            resolved_new, new_parent_path, sizeof(new_parent_path), new_base, sizeof(new_base)
        ))
        return -1;

    vfs_node_t* old_parent = vfs_lookup(old_parent_path);
    if (old_parent && VFS_IS_LINK(old_parent->type))
        old_parent = old_parent->link;

    vfs_node_t* new_parent = vfs_lookup(new_parent_path);
    if (new_parent && VFS_IS_LINK(new_parent->type))
        new_parent = new_parent->link;

    if (!old_parent || !new_parent || old_parent->type != VFS_DIR || new_parent->type != VFS_DIR)
        return -1;

    if (!vfs_access(old_parent, thread->uid, thread->gid, W_OK | X_OK))
        return -1;

    if (!vfs_access(new_parent, thread->uid, thread->gid, W_OK | X_OK))
        return -1;

    return vfs_rename(resolved_old, resolved_new) ? 0 : -1;
}

static off_t _sys_seek(int fd, off_t offset, int whence) {
    sched_thread_t* thread = sched_current();
    if (!thread || fd < 0 || fd >= SCHED_FD_MAX || !thread->fd_used[fd])
        return -1;

    sched_fd_t* entry = &thread->fds[fd];
    if (!entry->node)
        return -1;

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
        return -1;
    }

    off_t next = base + offset;
    if (next < 0)
        return -1;

    entry->offset = (size_t)next;
    return next;
}

static int _sys_getdents(int fd, dirent_t* out) {
    if (!out || !_valid_user_ptr(out, sizeof(dirent_t)))
        return -1;

    sched_thread_t* thread = sched_current();
    if (!thread || fd < 0 || fd >= SCHED_FD_MAX || !thread->fd_used[fd])
        return -1;

    sched_fd_t* entry = &thread->fds[fd];
    vfs_node_t* node = entry->node;

    if (node && VFS_IS_LINK(node->type) && node->link)
        node = node->link;

    if (!node || node->type != VFS_DIR || !node->tree_entry)
        return -1;

    size_t index = entry->offset;
    size_t current = 0;

    ll_foreach(child, node->tree_entry->children) {
        if (current++ != index)
            continue;

        tree_node_t* tnode = child->data;
        if (!tnode)
            return -1;

        vfs_node_t* vnode = tnode->data;
        if (!vnode)
            return -1;

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

static int _sys_sleep(unsigned int seconds) {
    u32 hz = arch_timer_hz();
    if (hz == 0)
        return -1;

    u64 ticks = (u64)seconds * (u64)hz;
    sched_sleep(ticks);
    return 0;
}

static uintptr_t _sys_signal(int signum, sighandler_t handler, uintptr_t trampoline) {
    sched_thread_t* thread = sched_current();
    if (!thread)
        return (uintptr_t)SIG_ERR;

    return (uintptr_t)sched_signal_set_handler(thread, signum, handler, trampoline);
}

static u64 _sys_sigreturn(arch_int_state_t* state) {
    sched_thread_t* thread = sched_current();
    if (!thread || !state)
        return (u64)-1;

    return sched_signal_sigreturn(thread, state) ? 0 : (u64)-1;
}

static u64 _sys_kill(pid_t pid, int signum) {
    return (u64)sched_signal_send_pid(pid, signum);
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
    case SYS_PREAD:
        return (u64)_sys_pread(
            (int)arch_syscall_arg1(state),
            (void*)arch_syscall_arg2(state),
            (size_t)arch_syscall_arg3(state),
            (off_t)arch_syscall_arg4(state)
        );
    case SYS_WRITE:
        return (u64)_write(
            (int)arch_syscall_arg1(state),
            (void*)arch_syscall_arg2(state),
            (size_t)arch_syscall_arg3(state)
        );
    case SYS_PWRITE:
        return (u64)_sys_pwrite(
            (int)arch_syscall_arg1(state),
            (void*)arch_syscall_arg2(state),
            (size_t)arch_syscall_arg3(state),
            (off_t)arch_syscall_arg4(state)
        );
    case SYS_OPEN:
        return (u64)sys_open(
            (const char*)arch_syscall_arg1(state),
            (int)arch_syscall_arg2(state),
            (mode_t)arch_syscall_arg3(state)
        );
    case SYS_CLOSE:
        return (u64)_sys_close((int)arch_syscall_arg1(state));
    case SYS_SEEK:
        return (u64)_sys_seek(
            (int)arch_syscall_arg1(state),
            (off_t)arch_syscall_arg2(state),
            (int)arch_syscall_arg3(state)
        );
    case SYS_GETDENTS:
        return (u64)sys_getdents((int)arch_syscall_arg1(state), (dirent_t*)arch_syscall_arg2(state));
    case SYS_MKDIR:
        return (u64)_sys_mkdir(
            (const char*)arch_syscall_arg1(state), (mode_t)arch_syscall_arg2(state)
        );
    case SYS_ACCESS:
        return (u64)_sys_access((const char*)arch_syscall_arg1(state), (int)arch_syscall_arg2(state));
    case SYS_CHDIR:
        return (u64)_sys_chdir((const char*)arch_syscall_arg1(state));
    case SYS_GETCWD:
        return (u64)sys_getcwd((char*)arch_syscall_arg1(state), (size_t)arch_syscall_arg2(state));
    case SYS_GETUID:
        return (u64)_sys_getuid();
    case SYS_GETGID:
        return (u64)_sys_getgid();
    case SYS_SETUID:
        return (u64)_sys_setuid((uid_t)arch_syscall_arg1(state));
    case SYS_SETGID:
        return (u64)sys_setgid((gid_t)arch_syscall_arg1(state));
    case SYS_SETPGID:
        return (u64)_sys_setpgid((pid_t)arch_syscall_arg1(state), (pid_t)arch_syscall_arg2(state));
    case SYS_GETPGID:
        return (u64)_sys_getpgid((pid_t)arch_syscall_arg1(state));
    case SYS_SETSID:
        return (u64)_sys_setsid();
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
    case SYS_SLEEP:
        return (u64)sys_sleep((unsigned int)arch_syscall_arg1(state));
    case SYS_IOCTL:
        return (u64)_ioctl(
            (int)arch_syscall_arg1(state),
            (u64)arch_syscall_arg2(state),
            (void*)arch_syscall_arg3(state)
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
    }
    case SYS_GETPID: {
        sched_thread_t* thread = sched_current();
        return thread ? (u64)thread->pid : (u64)-1;
    }
    case SYS_GETPPID: {
        sched_thread_t* thread = sched_current();
        return thread ? (u64)thread->ppid : (u64)-1;
    }
    default:
        return (u64)-1;
    }
}

static void _handler(arch_int_state_t* state) {
    if (!state)
        return;

    u64 ret = _dispatch(state);

    arch_syscall_set_ret(state, (arch_syscall_t)ret);
}

void syscall_init(void) {
    arch_syscall_install(SYSCALL_INT, _handler);
    log_debug("syscall: interface initialized");
}
