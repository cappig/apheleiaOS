#include <stdarg.h>
#include <sys/ioctl.h>
#include <unistd.h>


int ioctl(int fd, unsigned long request, ...) {
    void* args;
    va_list ap;

    va_start(ap, request);

    args = va_arg(ap, void*);

    va_end(ap);

    return syscall3(SYS_IOCTL, fd, request, (u64)args);
}
