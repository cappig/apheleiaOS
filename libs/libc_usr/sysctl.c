#include <arch/sys.h>
#include <errno.h>
#include <stdint.h>
#include <sys/sysctl.h>
#include <unistd.h>

ssize_t sysctl(const char* name, void* out, size_t len) {
    return (ssize_t)__SYSCALL_ERRNO(
        syscall3(SYS_SYSCTL, (uintptr_t)name, (uintptr_t)out, (uintptr_t)len)
    );
}
