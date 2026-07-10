#include <apheleia/syscall.h>
#include <arch/sys.h>
#include <errno.h>
#include <utime.h>

int utime(const char *filename, const struct utimbuf *times) {
    return (int)__SYSCALL_ERRNO(syscall2(SYS_UTIME, (uintptr_t)filename, (uintptr_t)times));
}
