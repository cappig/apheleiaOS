#include "syscall.h"

#include <arch/arch.h>
#include <arch/syscall.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sys/exec.h>
#include <sys/tty.h>
#include <unistd.h>

#define SYSCALL_INT 0x80

#if defined(__x86_64__)
#define KERNEL_BASE 0xFFFFFFFF80000000ULL
#else
#define KERNEL_BASE 0xC0000000U
#endif

static bool _valid_user_ptr(const void* ptr, size_t size) {
    uintptr_t addr = (uintptr_t)ptr;
    return addr + size <= KERNEL_BASE && addr + size >= addr;
}

static ssize_t _read(int fd, void* buf, size_t len) {
    if (!buf || !len)
        return 0;

    if (!_valid_user_ptr(buf, len))
        return -1;

    if (fd != STDIN_FILENO)
        return -1;

    tty_handle_t handle = {.kind = TTY_HANDLE_CURRENT, .index = 0};
    return tty_read_handle(&handle, buf, len);
}

static ssize_t _write(int fd, const void* buf, size_t len) {
    if (!buf || !len)
        return 0;

    if (!_valid_user_ptr(buf, len))
        return -1;

    if (fd != STDOUT_FILENO && fd != STDERR_FILENO && fd != STDIN_FILENO)
        return -1;

    tty_handle_t handle = {.kind = TTY_HANDLE_CURRENT, .index = 0};
    return tty_write_handle(&handle, buf, len);
}

static ssize_t _ioctl(int fd, u64 request, void* args) {
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

    vfs_node_t* node = NULL;
    if (flags & O_CREAT)
        node = vfs_open(resolved, VFS_FILE, true, mode);
    else
        node = vfs_lookup(resolved);

    if (!node)
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

static int _sys_chdir(const char* path) {
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
    case SYS_WRITE:
        return (u64)_write(
            (int)arch_syscall_arg1(state),
            (void*)arch_syscall_arg2(state),
            (size_t)arch_syscall_arg3(state)
        );
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
