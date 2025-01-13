#include "syscall.h"

#include <aos/syscalls.h>
#include <base/addr.h>
#include <base/types.h>
#include <data/vector.h>
#include <errno.h>
#include <log/log.h>
#include <x86/paging.h>

#include "arch/gdt.h"
#include "arch/idt.h"
#include "mem/virtual.h"
#include "sched/process.h"
#include "sched/scheduler.h"
#include "sys/tty.h"
#include "vfs/fs.h"


static inline file_desc* _get_fd(usize fd) {
    vector* fd_table = sched_instance.current->user.fd_table;

    return vec_at(fd_table, fd);
}

static bool _valid_fd(usize fd) {
    file_desc* fdesc = _get_fd(fd);

    if (!fdesc)
        return NULL;

    return fdesc->node;
}

static bool _valid_ptr(const void* ptr, usize len, bool write) {
    if (!ptr)
        return false;

    if ((u64)ptr > LOWER_HALF_TOP)
        return false;

    void* cur = (void*)ptr;

    while (cur < ptr + len) {
        page_table* page;
        usize size = get_page(sched_instance.current->user.mem_map, (u64)ptr, &page);

        if (!size || !page)
            return false;

        if (!page->bits.present)
            return false;

        if (!page->bits.user)
            return false;

        if (write && !page->bits.writable)
            return false;

        cur += size;
    }

    return true;
}

static bool _valid_signum(usize signum) {
    if (signum >= SIGNAL_COUNT)
        return false;

    return true;
}


static void _exit(u64 status) {
    scheduler_kill(sched_instance.current, status);
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

    isize fd = process_open_fd(sched_instance.current, path, -1);

    return fd;
}


static u64 _getpid(void) {
    return sched_instance.current->id;
}


static void _syscall_handler(int_state* s) {
#ifdef SYSCALL_DEBUG
    log_debug("[SYSCALL_DEBUG] handling syscall rax = %#lx", s->g_regs.rax);
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


    case SYS_GETPID:
        ret = _getpid();
        break;

    case SYS_SIGNAL:
    case SYS_SIGRETURN:
    case SYS_KILL:
        // TODO: signals
    default:
#ifdef SYSCALL_DEBUG
        log_warn("[SYSCALL_DEBUG] Invalid syacall (%#lx)", s->g_regs.rax);
#endif
        break;
    }

    if (ret)
        s->g_regs.rax = ret;
}


void syscall_init() {
    configure_int(SYSCALL_INT, GDT_kernel_code, 0, 0xef);
    set_int_handler(SYSCALL_INT, _syscall_handler);

    log_info("Initialised the syscall interface");
}
