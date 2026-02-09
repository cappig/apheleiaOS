#include "syscall.h"

#include <arch/arch.h>
#include <arch/syscall.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sys/exec.h>
#include <sys/tty.h>
#include <unistd.h>
#include <x86/gdt.h>
#include <x86/idt.h>

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

static u64 _dispatch(int_state_t* state) {
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

static void _handler(int_state_t* state) {
    if (!state)
        return;

    u64 ret = _dispatch(state);

    arch_syscall_set_ret(state, (arch_syscall_t)ret);
}

void syscall_init(void) {
    set_int_handler(SYSCALL_INT, _handler);
    configure_int(SYSCALL_INT, GDT_KERNEL_CODE, 0, IDT_TRP);
    log_debug("syscall: interface initialized");
}
