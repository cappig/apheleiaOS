#include <arch/sys.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

int ioctl(int fd, unsigned long request, ...) {
    va_list args;
    void* argp = NULL;

    va_start(args, request);
    argp = va_arg(args, void*);
    va_end(args);

    return (int)__SYSCALL_ERRNO(
        syscall3(SYS_IOCTL, (uintptr_t)fd, (uintptr_t)request, (uintptr_t)argp)
    );
}
