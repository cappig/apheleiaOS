#include <apheleia/syscall.h>
#include <arch/sys.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>

int fcntl(int fd, int cmd, ...) {
    uintptr_t arg = 0;

    if (
        cmd == F_DUPFD ||
        cmd == F_DUPFD_CLOEXEC ||
        cmd == F_SETFD ||
        cmd == F_SETFL
    ) {
        va_list ap;
        va_start(ap, cmd);
        arg = (uintptr_t)va_arg(ap, int);
        va_end(ap);
    }

    return (int)__SYSCALL_ERRNO(
        syscall3(SYS_FCNTL, (uintptr_t)fd, (uintptr_t)cmd, arg)
    );
}
