#include <arch/sys.h>
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>

int getdents(int fd, dirent_t* out) {
    return (int)__SYSCALL_ERRNO(syscall2(SYS_GETDENTS, (uintptr_t)fd, (uintptr_t)out));
}
