#include <fcntl.h>
#include <stdarg.h>
#include <sys/types.h>
#include <unistd.h>

#include "errno.h"


int open(const char* path, int flags, ...) {
    mode_t mode = 0;

    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }

    return __SYSCALL_ERRNO(syscall3(SYS_OPEN, (u64)path, flags, mode));
}
