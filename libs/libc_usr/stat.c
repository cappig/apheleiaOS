#include <arch/sys.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int stat(const char *path, stat_t *st) {
    return (int)__SYSCALL_ERRNO(syscall2(SYS_STAT, (uintptr_t)path, (uintptr_t)st));
}

int lstat(const char *path, stat_t *st) {
    return (int)__SYSCALL_ERRNO(syscall2(SYS_LSTAT, (uintptr_t)path, (uintptr_t)st));
}

int fstat(int fd, stat_t *st) {
    return (int)__SYSCALL_ERRNO(syscall2(SYS_FSTAT, (uintptr_t)fd, (uintptr_t)st));
}

int chmod(const char *path, mode_t mode) {
    return (int)__SYSCALL_ERRNO(syscall2(SYS_CHMOD, (uintptr_t)path, (uintptr_t)mode));
}

int chown(const char *path, uid_t uid, gid_t gid) {
    return (int)__SYSCALL_ERRNO(
        syscall3(SYS_CHOWN, (uintptr_t)path, (uintptr_t)uid, (uintptr_t)gid)
    );
}
