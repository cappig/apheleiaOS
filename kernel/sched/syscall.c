#include "syscall.h"

#include <base/addr.h>
#include <base/types.h>
#include <data/list.h>
#include <data/tree.h>
#include <data/vector.h>
#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <x86/paging.h>
#include <x86/sys.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "mem/heap.h"
#include "sched/exec.h"
#include "sched/process.h"
#include "sched/scheduler.h"
#include "sched/signal.h"
#include "sys/cpu.h"
#include "vfs/fs.h"
#include "x86/asm.h"


static void _exit(u64 status) {
    sched_process* proc = cpu_current_proc();

    sched_dequeue_proc(proc);
    proc_terminate(proc, status);
}


static isize __read(u64 fd, u64 buf_ptr, u64 len, u64 offset, bool use_fd_offset) {
    void* buf = (void*)buf_ptr;

    if (!validate_fd(fd))
        return -EBADF;

    if (!validate_ptr(buf, len, false))
        return -EFAULT;

    file_desc* fdesc = get_fd(fd);

    if (!(fdesc->flags & FD_READ))
        return -EINVAL;

    vfs_node* node = fdesc->node;

    if (!node)
        return -EBADF;

    if (node->type == VFS_DIR)
        return -EISDIR;

    if (use_fd_offset)
        offset = fdesc->offset;

    usize flags = 0;

    if (fdesc->flags & FD_NONBLOCK)
        flags |= VFS_NONBLOCK;

    isize read = vfs_read(node, buf, offset, len, flags);

    if (read >= 0 && use_fd_offset)
        fdesc->offset += read;

    return read;
}

static isize _read(u64 fd, u64 buf_ptr, u64 len) {
    return __read(fd, buf_ptr, len, 0, true);
}

static isize _pread(u64 fd, u64 buf_ptr, u64 len, u64 offset) {
    return __read(fd, buf_ptr, len, offset, false);
}


static isize __write(u64 fd, u64 buf_ptr, u64 len, u64 offset, bool use_fd_offset) {
    void* buf = (void*)buf_ptr;

    if (!validate_fd(fd))
        return -EBADF;

    if (!validate_ptr(buf, len, false))
        return -EFAULT;

    file_desc* fdesc = get_fd(fd);

    if (!(fdesc->flags & FD_WRITE))
        return -EINVAL;

    vfs_node* node = fdesc->node;

    if (!node)
        return -EBADF;

    if (use_fd_offset)
        offset = fdesc->offset;

    usize flags = 0;

    if (fdesc->flags & FD_NONBLOCK)
        flags |= VFS_NONBLOCK;

    isize written = vfs_write(node, buf, offset, len, flags);

    if (written > 0 && use_fd_offset)
        fdesc->offset += written;

    return written;
}

static isize _write(u64 fd, u64 buf_ptr, u64 len) {
    return __write(fd, buf_ptr, len, 0, true);
}

static isize _pwrite(u64 fd, u64 buf_ptr, u64 len, u64 offset) {
    return __write(fd, buf_ptr, len, offset, false);
}


static isize _seek(int fd, off_t offset, int whence) {
    if (!validate_fd(fd))
        return -EBADF;

    file_desc* fdesc = get_fd(fd);

    if (!fdesc)
        return -EBADF;

    vfs_node* node = fdesc->node;

    if (!node)
        return -EBADF;

    if (node->type != VFS_FILE)
        return -ESPIPE;

    isize new_off;

    switch (whence) {
    case SEEK_SET:
        new_off = offset;
        break;

    case SEEK_CUR:
        new_off = fdesc->offset + offset;
        break;

    case SEEK_END:
        new_off = fdesc->node->size + offset;
        break;

    default:
        return -EINVAL;
    }

    if (new_off < 0)
        return -EINVAL;

    fdesc->offset = new_off;

    return fdesc->offset;
}


// TODO: finish this
static isize _open(u64 path_ptr, u64 flags, u64 mode) {
    const char* path = (const char*)path_ptr;

    if (!validate_ptr(path, 1, false))
        return -EFAULT;

    vfs_node* node = vfs_open(path, VFS_FILE, flags & O_CREAT, mode); // TODO: mode

    if (!node)
        return -ENOENT;

    u16 fd_flags = 0;

    if (flags & O_RDONLY || flags & O_RDWR)
        fd_flags |= FD_READ;

    if (flags & O_WRONLY || flags & O_RDWR)
        fd_flags |= FD_WRITE;

    if (flags & O_APPEND)
        fd_flags |= FD_APPEND;

    if (flags & O_NONBLOCK)
        fd_flags |= FD_NONBLOCK;

    isize fd = proc_open_fd_node(cpu_current_proc(), node, -1, fd_flags);
    file_desc* fdesc = get_fd(fd);

    if (!fdesc) // wtf?
        return -EFAULT;

    return fd;
}

static u64 _close(usize fd) {
    if (!validate_fd(fd))
        return -1;

    sched_process* proc = cpu_current_proc();

    // the file_desc only referances a node in the vfs tree
    file_desc* fdesc = process_get_fd(proc, fd);
    memset(fdesc, 0, sizeof(file_desc));

    return 0;
}

static u64 _mkdir(u64 path_ptr, u64 mode) {
    const char* path = (const char*)path_ptr;

    if (!validate_ptr(path, 1, false))
        return -EFAULT;

    char* path_cpy = strdup(path);

    char* dir_name = dirname(path_cpy);
    char* base_name = basename(path_cpy);

    vfs_node* parent = vfs_lookup(dir_name);
    if (!parent)
        return -errno;

    vfs_node* child = vfs_create(parent, base_name, VFS_DIR, mode);

    sched_process* proc = cpu_current_proc();
    child->uid = proc->identity.euid;

    kfree(path_cpy);

    if (!child)
        return -EFAULT;

    return 0;
}

static u64 _access(u64 path_ptr, u64 mode) {
    const char* path = (const char*)path_ptr;

    if (!validate_ptr(path, 1, false))
        return -EFAULT;

    vfs_node* vnode = vfs_lookup(path);
    if (!vnode)
        return -errno;

    sched_process* current = cpu_current_proc();

    bool ret = vfs_access(vnode, current->identity.uid, current->identity.gid, mode);

    return ret ? 0 : -EACCES;
}

static u64 _ioctl(u64 fd, u64 request, u64 arg) {
    file_desc* fdesc = get_fd(fd);

    if (!fdesc)
        return -EBADF;

    vfs_node* node = fdesc->node;

    if (!node)
        return -EBADF;

    if (node->type != VFS_CHARDEV)
        return -ENOTTY;

    if (!node->interface || !node->interface->ioctl)
        return -ENOTTY;

    node->interface->ioctl(node, request, (void*)arg);

    return 0;
}

static u64 _signal(u64 signum, u64 handler_ptr) {
    sighandler_t handler = (sighandler_t)handler_ptr;

    if (handler != SIG_IGN && handler != SIG_DFL)
        if (!validate_ptr(handler, 1, false))
            return (u64)SIG_ERR;

    sched_process* proc = cpu_current_proc();
    u64 prev = (u64)proc->signals.handlers[signum - 1];

    if (!proc_signal_set_handler(proc, signum, handler))
        return (u64)SIG_ERR;

    return prev;
}

static u64 _sigreturn(void) {
    sched_thread* thread = cpu_current_thread();
    sched_process* proc = thread->proc;

    if (thread->proc->type != PROC_USER)
        return -ENODATA;

    if (!thread_signal_return(thread))
        signal_send(proc, thread->tid, SIGILL);

    return 0;
}

static u64 _kill(u64 pid, int signum) {
    int ret = -1;

    // signal a specific process
    if (signum > 0) {
        sched_process* proc = sched_get_proc(pid);

        if (!proc)
            return -ESRCH;

        if (!has_perms(proc->identity.uid))
            return -EPERM;

        ret = signal_send(proc, -1, signum);
    }
    // signal all processes in the senders process group
    else if (signum == 0) {
        sched_process* current = cpu_current_proc();

        if (!current->group)
            return -ESRCH;

        ret = signal_send_group(current->group, signum);

    }
    // signal all processes the sender is allowed to signal
    else if (signum == -1) {
        // TODO: implement this
    }
    // signal a specific process group
    else if (signum < -1) {
        ret = signal_send_group(-signum, signum);
    }

    return (ret < 0) ? -1 : 0;
}

static u64 _wait(pid_t pid, u64 status_ptr, u64 options) {
    if (!validate_ptr((void*)status_ptr, sizeof(int*), true))
        return -EFAULT;

    int* status = (int*)status_ptr;
    sched_thread* thread = cpu_current_thread();
    sched_process* proc = thread->proc;

    // TODO: group ids
    if (pid < -1 || pid == 0)
        return -EINVAL;

    if (pid > 0) {
        // Wait for a specific child
        sched_process* target_proc = sched_get_proc(pid);

        if (!target_proc)
            return -ESRCH;

        // Is this process the child of the caller
        tree_node* parent_node = proc->tnode->parent;

        if (!parent_node)
            return -EINVAL;

        sched_process* parent = parent_node->data;

        if (parent != proc)
            return -ECHILD;

        // Is the process done already?
        if (proc->state == PROC_ZOMBIE) {
            *status = proc->exit_code; // the page map is loaded at this point

            proc_destroy(proc);
            return proc->pid;
        }
    } else {
        //  Wait for any child
        foreach (node, proc->tnode->children) {
            tree_node* child_node = node->data;
            sched_process* child = child_node->data;

            // Are any children done already?
            if (child->state == PROC_ZOMBIE) {
                *status = child->exit_code;

                pid_t child_pid = child->pid;
                proc_destroy(child);

                return child_pid;
            }
        }
    }

    // Not done but we don't want to hang
    if (options & WNOHANG)
        return 0;

    // Not done and we do want to hang
    // This hangs only the calling thread
    sched_dequeue(thread, true);

    thread->waiting.proc = pid;
    thread->waiting.code_ptr = status;

    /* cpu->scheduler.needs_resched = true; */

    return 0;
}


static u64 _getpid(void) {
    return cpu_current_proc()->pid;
}

static u64 _getppid(void) {
    sched_process* proc = cpu_current_proc();
    tree_node* parent_node = proc->tnode->parent;

    // This means that init called
    if (!parent_node)
        return 0;

    sched_process* parent = parent_node->data;
    return parent->pid;
}

u64 _setpgid(u64 pid, u64 pgid) {
    sched_process* current = cpu_current_proc();

    if (!pid)
        pid = current->pid;

    if (!pgid)
        pgid = pid;

    sched_process* target = sched_get_proc(pid);

    if (!target)
        return -ESRCH;

    if (target->session != current->session)
        return -EPERM;

    if (!proc_is_descendant(target, current))
        return -EPERM;

    target->group = pgid;

    return 0;
}

u64 _getpgid(u64 pid) {
    sched_process* current = cpu_current_proc();

    if (!pid)
        pid = current->pid;

    sched_process* target = sched_get_proc(pid);

    if (!target)
        return -ESRCH;

    return target->group;
}

u64 _setsid(void) {
    sched_process* current = cpu_current_proc();

    if (current->pid == current->group)
        return -EPERM;

    current->group = current->pid;
    current->session = current->group;

    return current->group;
}


static u64 _fork(void) {
    sched_thread* thread = cpu_current_thread();
    sched_process* proc = thread->proc;

    sched_process* child = proc_fork(proc, thread->tid);
    sched_thread* child_thread = proc_get_thread(child, 0);

    // When the child returns it will get 0
    int_state* child_state = (int_state*)child_thread->kstack.ptr;
    child_state->g_regs.rax = 0;

    sched_enqueue_proc(child);

    // And when the parent returns it will get the pid of the child
    return child->pid;
}

static u64 _execve(u64 path_ptr, u64 argv_ptr, u64 envp_ptr) {
    char* path = (char*)path_ptr;
    char** argv = (char**)argv_ptr;
    char** envp = (char**)envp_ptr;

    if (!path)
        return -EINVAL;

    // FIXME: the len should be taken into account here
    if (!validate_ptr(path, 1, false))
        return -EFAULT;

    if (argv && !validate_ptr(argv, 1, false))
        return -EFAULT;

    if (envp && !validate_ptr(envp, 1, false))
        return -EFAULT;

    vfs_node* file = vfs_lookup(path);

    if (!file)
        return -EFAULT;

    sched_thread* thread = cpu_current_thread();

    return exec_elf(thread, file, argv, envp);
}

static u64 _sleep(u64 milis) {
    sched_thread* thread = cpu_current_thread();

    if (milis)
        sched_thread_sleep(thread, milis);

    return 0;
}

static u64 _mount(u64 source_ptr, u64 target_ptr, u64 flags) {
    // Only the superuser is allowed to mount disks
    if (!has_perms(SUPERUSER_UID))
        return -EPERM;

    const char* source = (const char*)source_ptr;
    const char* target = (const char*)target_ptr;

    // FIXME: What if one half of the string resides in a valid page and the rest resides in an
    // invalid page? This would return true and we would get a PF/GP exception here in the kernel
    if (!validate_ptr(source, 1, false))
        return -EFAULT;

    if (!validate_ptr(target, 1, false))
        return -EFAULT;

    vfs_node* source_vnode = vfs_lookup(source);
    if (!source_vnode)
        return -ENODEV;

    if (source_vnode->type != VFS_BLOCKDEV)
        return -ENODEV;

    if (!source_vnode->fs)
        return -ENODEV;

    vfs_node* target_vnode = vfs_lookup(target);
    if (!target_vnode)
        return -ENODEV;

    if (target_vnode->type != VFS_DIR)
        return -ENOTDIR;

    if (!vfs_mount(source_vnode->fs, target_vnode))
        return -EFAULT;

    return 0;
}

static u64 _unmount(u64 target_ptr, u64 flags) {
    if (!has_perms(SUPERUSER_UID))
        return -EPERM;

    const char* target = (const char*)target_ptr;

    if (!validate_ptr(target, 1, false))
        return -EFAULT;

    vfs_node* target_vnode = vfs_lookup(target);
    if (!target_vnode)
        return -errno;

    // TODO: destroy_tree by default?
    if (!vfs_unmount(target_vnode, false))
        return -EFAULT;

    return 0;
}


static u64 _mmap(u64 addr, size_t len, int prot, int flags, int fd, off_t offset) {
    sched_process* proc = cpu_current_proc();

    if (!len)
        return -EINVAL;

    file_desc* fdesc = NULL;

    if (!(flags & MAP_ANONYMOUS)) {
        fdesc = process_get_fd(proc, fd);

        if (!fdesc)
            return -EBADF;

        if (prot & PROT_WRITE) {
            if (!(fdesc->flags & FD_WRITE))
                return -EACCES;

            if (fdesc->flags & FD_APPEND)
                return -EACCES;
        }

        if (prot & PROT_READ) {
            if (!(fdesc->flags & FD_READ))
                return -EACCES;
        }

        if (VFS_IS_DEVICE(fdesc->node->type)) {
            if (!fdesc->node->interface->mmap)
                return -EINVAL;

            // FIXME: (TODO) implement mmaping device files!!
            return -EINVAL;
        }
    }

    return proc_mmap(proc, addr, len, prot, flags, fdesc ? fdesc->node : NULL, offset);
}


static void _syscall_handler(int_state* s) {
#ifdef SYSCALL_DEBUG
    log_debug(
        "[SYSCALL_DEBUG] handling syscall %lu (PID: %zd)",
        s->g_regs.rax,
        cpu->scheduler.current->proc->pid
    );
#endif

    u64 arg1 = s->g_regs.rdi;
    u64 arg2 = s->g_regs.rsi;
    u64 arg3 = s->g_regs.rdx;
    u64 arg4 = s->g_regs.rcx;
    u64 arg5 = s->g_regs.r8;
    u64 arg6 = s->g_regs.r9;

    u64 ret = 0;

    switch (s->g_regs.rax) {
    case SYS_EXIT:
        _exit(arg1);
        break;

    case SYS_READ:
        ret = _read(arg1, arg2, arg3);
        break;
    case SYS_PREAD:
        ret = _pread(arg1, arg2, arg3, arg4);
        break;

    case SYS_WRITE:
        ret = _write(arg1, arg2, arg3);
        break;
    case SYS_PWRITE:
        ret = _pwrite(arg1, arg2, arg3, arg4);
        break;

    case SYS_SEEK:
        ret = _seek(arg1, arg2, arg3);
        break;

    case SYS_OPEN:
        ret = _open(arg1, arg2, arg3);
        break;

    case SYS_CLOSE:
        ret = _close(arg1);
        break;

    case SYS_MKDIR:
        ret = _mkdir(arg1, arg2);
        break;

    case SYS_ACCESS:
        ret = _access(arg1, arg2);
        break;

    case SYS_IOCTL:
        ret = _ioctl(arg1, arg2, arg3);
        break;


    case SYS_SIGNAL:
        ret = _signal(arg1, arg2);
        break;

    case SYS_SIGRETURN:
        ret = _sigreturn();
        break;


    case SYS_KILL:
        ret = _kill(arg1, arg2);
        break;

    case SYS_WAIT:
        ret = _wait(arg1, arg2, arg3);
        break;


    case SYS_GETPID:
        ret = _getpid();
        break;

    case SYS_GETPPID:
        ret = _getppid();
        break;

    case SYS_SETPGID:
        ret = _setpgid(arg1, arg2);
        break;

    case SYS_GETPGID:
        ret = _getpgid(arg1);
        break;

    case SYS_SETSID:
        ret = _setsid();
        break;


    case SYS_FORK:
        ret = _fork();
        break;

    case SYS_EXECVE:
        ret = _execve(arg1, arg2, arg3);
        break;


    case SYS_SLEEP:
        ret = _sleep(arg1);
        break;


    case SYS_MOUNT:
        ret = _mount(arg1, arg2, arg3);
        break;

    case SYS_UNMOUNT:
        ret = _unmount(arg1, arg2);
        break;


    case SYS_MMAP:
        ret = _mmap(arg1, arg2, arg3, arg4, arg5, arg6);
        break;


    default:
        sched_thread* thread = cpu_current_thread();
        signal_send(thread->proc, thread->tid, SIGSYS);
        break;
    }

#ifdef SYSCALL_DEBUG
    log_debug("[SYSCALL_DEBUG] syscall %lu returning with %d", s->g_regs.rax, ret);
#endif

    // FIXME: the biggest problem in the kernel right now is its shameless reliannce on x86
    // The kernel should be cpu agnostic
    s->g_regs.rax = ret;
}


void syscall_init() {
    configure_int(SYSCALL_INT, GDT_kernel_code, 0, 0xef);
    set_int_handler(SYSCALL_INT, _syscall_handler);

    log_debug("Initialised the syscall interface");
}
