#include "syscall.h"

#include <aos/signals.h>
#include <aos/syscalls.h>
#include <base/addr.h>
#include <base/types.h>
#include <data/list.h>
#include <data/vector.h>
#include <errno.h>
#include <log/log.h>
#include <string.h>
#include <x86/paging.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "sched/exec.h"
#include "sched/process.h"
#include "sched/scheduler.h"
#include "sched/signal.h"
#include "sys/cpu.h"
#include "vfs/fs.h"


static inline file_desc* _get_fd(usize fd) {
    vector* fd_table = cpu->sched->current->user.fd_table;

    return vec_at(fd_table, fd);
}

static bool _valid_fd(usize fd) {
    file_desc* fdesc = _get_fd(fd);

    if (!fdesc)
        return NULL;

    return fdesc->node;
}

static bool _valid_ptr(const void* ptr, usize len, bool write) {
    return process_validate_ptr(cpu->sched->current, ptr, len, write);
}

static bool _valid_signum(usize signum) {
    if (signum >= SIGNAL_COUNT)
        return false;

    return true;
}


static void _exit(u64 status) {
    scheduler_kill(cpu->sched->current, status);
}


static isize __read(u64 fd, u64 buf_ptr, u64 len, u64 offset, bool use_fd_offset) {
    void* buf = (void*)buf_ptr;

    if (!_valid_fd(fd))
        return -EBADF;

    if (!_valid_ptr(buf, len, false))
        return -EFAULT;

    file_desc* fdesc = _get_fd(fd);
    vfs_node* node = fdesc->node;

    if (use_fd_offset)
        offset = fdesc->offset;

    if (!node)
        return -EBADF;

    if (node->type == VFS_DIR)
        return -EISDIR;

    if (!node->interface)
        return -EBADF;

    if (!node->interface->read)
        return -EBADF;

    isize read = node->interface->read(node, buf, offset, len);

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

    if (!_valid_fd(fd))
        return -EBADF;

    if (!_valid_ptr(buf, len, true))
        return -EFAULT;

    file_desc* fdesc = _get_fd(fd);

    vfs_node* node = fdesc->node;

    if (use_fd_offset)
        offset = fdesc->offset;

    if (!node)
        return -EBADF;

    if (!node->interface->write)
        return -EBADF;

    isize written = node->interface->write(node, buf, offset, len);

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
    if (!_valid_fd(fd))
        return -EBADF;

    file_desc* fdesc = _get_fd(fd);
    vfs_node* node = fdesc->node;

    if (!node)
        return -EBADF;

    if (node->type != VFS_FILE)
        return -ESPIPE;

    isize new_off;

    switch (whence) {
    case SYS_SEEK_SET:
        new_off = offset;
        break;

    case SYS_SEEK_CUR:
        new_off = fdesc->offset + offset;
        break;

    case SYS_SEEK_END:
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

    if (!_valid_ptr(path, 0, false))
        return -EFAULT;

    isize fd = process_open_fd(cpu->sched->current, path, -1);

    return fd;
}


static u64 _signal(u64 signum, u64 handler_ptr) {
    sighandler_t handler = (sighandler_t)handler_ptr;

    if (handler != SIG_IGN && handler != SIG_DFL)
        if (!_valid_ptr(handler, 0, false))
            return (u64)SIG_ERR;

    u64 prev = (u64)cpu->sched->current->user.signals.handlers[signum - 1];

    if (!signal_set_handler(cpu->sched->current, signum, handler))
        return (u64)SIG_ERR;

    return prev ? prev : (u64)SIG_DFL;
}

static u64 _sigreturn(void) {
    if (cpu->sched->current->type != PROC_USER)
        return -ENODATA;

    signal_return(cpu->sched->current);

    return 0;
}

static u64 _kill(u64 pid, u64 signum) {
    if (!_valid_signum(signum))
        return -EINVAL;

    process* proc = process_with_pid(pid);

    if (!proc)
        return -ESRCH;

    // TODO: check perms

    if (signum != 0)
        signal_send(proc, signum);

    return 0;
}

static u64 _wait(pid_t pid, u64 status_ptr, u64 options) {
    if (!_valid_ptr((void*)status_ptr, sizeof(int*), true))
        return -EFAULT;

    int* status = (int*)status_ptr;
    process* parent = cpu->sched->current;

    // TODO: group ids
    if (pid < -1 || pid == 0)
        return -EINVAL;

    if (pid > 0) {
        // Wait for a specific child
        process* proc = process_with_pid(pid);

        // Is this process the child of the caller
        process* real_parent = proc->user.tree_entry->data;
        if (parent != real_parent)
            return -ECHILD;

        // Is the process done already?
        if (proc->state == PROC_DONE) {
            // The process page map is loaded at this point
            *status = proc->status;
            return process_reap(proc);
        }
    } else {
        //  Wait for any child
        foreach (node, parent->user.tree_entry->children) {
            tree_node* child_node = node->data;
            process* child = child_node->data;

            // Are any children done already?
            if (child->state == PROC_DONE) {
                *status = child->status;
                return process_reap(child);
            }
        }
    }

    // Not done but we don't want to hang
    if (options & WNOHANG)
        return 0;

    // Not done and we do want to hang (a lot of unfortunate nomenclature in this one)
    parent->state = PROC_BLOCKED;
    parent->user.waiting = pid;
    parent->user.child_status = status;
    cpu->sched->proc_ticks_left = 0; // make sure that we get rescheduled

    return 0;
}


static u64 _getpid(void) {
    return cpu->sched->current->id;
}

static u64 _getppid(void) {
    tree_node* tnode = cpu->sched->current->user.tree_entry;

    // This means that init called
    if (!tnode->parent)
        return 0;

    process* parent = tnode->data;

    return parent->id;
}


static u64 _fork(void) {
    process* child = process_fork(cpu->sched->current);

    // When the child returns it will get 0
    int_state* child_state = (int_state*)child->stack_ptr;
    child_state->g_regs.rax = 0;

    scheduler_queue(child);

    // And when the parent returns it will get the pid of the child
    return child->id;
}

static u64 _sleep(u64 milis) {
    scheduler_sleep(cpu->sched->current, milis);
    cpu->sched->proc_ticks_left = 0;

    return 0;
}


static void _syscall_handler(int_state* s) {
#ifdef SYSCALL_DEBUG
    log_debug("[SYSCALL_DEBUG] handling syscall rax = %#lu", s->g_regs.rax);
#endif

    u64 arg1 = s->g_regs.rdi;
    u64 arg2 = s->g_regs.rsi;
    u64 arg3 = s->g_regs.rdx;
    u64 arg4 = s->g_regs.rcx;

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


    case SYS_FORK:
        ret = _fork();
        break;


    case SYS_SLEEP:
        ret = _sleep(arg1);
        break;


    default:
#ifdef SYSCALL_DEBUG
        log_warn("[SYSCALL_DEBUG] Invalid syacall (%lu)", s->g_regs.rax);
#endif
        signal_send(cpu->sched->current, SIGSYS);
        break;
    }

    s->g_regs.rax = ret;
}


void syscall_init() {
    configure_int(SYSCALL_INT, GDT_kernel_code, 0, 0xef);
    set_int_handler(SYSCALL_INT, _syscall_handler);

    log_info("Initialised the syscall interface");
}
