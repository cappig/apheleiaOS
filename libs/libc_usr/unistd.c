#include <arch/sys.h>
#include <errno.h>
#include <libc_usr/unistd.h>

#include <stdint.h>
#include <unistd.h>

#define SYSCALL_RET(type, expr) ((type)__SYSCALL_ERRNO(expr))

ssize_t read(int fd, void* buf, size_t count) {
    return SYSCALL_RET(
        ssize_t, syscall3(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count)
    );
}

ssize_t write(int fd, const void* buf, size_t count) {
    return SYSCALL_RET(
        ssize_t, syscall3(SYS_WRITE, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count)
    );
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    return SYSCALL_RET(
        ssize_t,
        syscall4(
            SYS_PREAD, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count, (uintptr_t)offset
        )
    );
}

ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
    return SYSCALL_RET(
        ssize_t,
        syscall4(
            SYS_PWRITE, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count, (uintptr_t)offset
        )
    );
}

int open(const char* path, int flags, mode_t mode) {
    return SYSCALL_RET(
        int, syscall3(SYS_OPEN, (uintptr_t)path, (uintptr_t)flags, (uintptr_t)mode)
    );
}

int close(int fd) {
    return SYSCALL_RET(int, syscall1(SYS_CLOSE, (uintptr_t)fd));
}

int mkdir(const char* path, mode_t mode) {
    return SYSCALL_RET(int, syscall2(SYS_MKDIR, (uintptr_t)path, (uintptr_t)mode));
}

int access(const char* path, int mode) {
    return SYSCALL_RET(int, syscall2(SYS_ACCESS, (uintptr_t)path, (uintptr_t)mode));
}

off_t lseek(int fd, off_t offset, int whence) {
    return SYSCALL_RET(
        off_t, syscall3(SYS_SEEK, (uintptr_t)fd, (uintptr_t)offset, (uintptr_t)whence)
    );
}

unsigned int sleep(unsigned int seconds) {
    long ret = syscall1(SYS_SLEEP, (uintptr_t)seconds);
    if (ret < 0) {
        errno = (int)-ret;
        return (unsigned int)-1;
    }
    return (unsigned int)ret;
}

int chdir(const char* path) {
    return SYSCALL_RET(int, syscall1(SYS_CHDIR, (uintptr_t)path));
}

char* getcwd(char* buf, size_t size) {
    if (!buf || !size)
        return NULL;

    long ret = __SYSCALL_ERRNO(syscall2(SYS_GETCWD, (uintptr_t)buf, (uintptr_t)size));
    return ret == 0 ? buf : NULL;
}

int link(const char* oldpath, const char* newpath) {
    return SYSCALL_RET(int, syscall2(SYS_LINK, (uintptr_t)oldpath, (uintptr_t)newpath));
}

int unlink(const char* path) {
    return SYSCALL_RET(int, syscall1(SYS_UNLINK, (uintptr_t)path));
}

int rename(const char* oldpath, const char* newpath) {
    return SYSCALL_RET(int, syscall2(SYS_RENAME, (uintptr_t)oldpath, (uintptr_t)newpath));
}

pid_t fork(void) {
    return SYSCALL_RET(pid_t, syscall0(SYS_FORK));
}

pid_t wait(pid_t pid, int* status) {
    return SYSCALL_RET(pid_t, syscall2(SYS_WAIT, (uintptr_t)pid, (uintptr_t)status));
}

pid_t waitpid(pid_t pid, int* status, int options) {
    return SYSCALL_RET(
        pid_t, syscall3(SYS_WAITPID, (uintptr_t)pid, (uintptr_t)status, (uintptr_t)options)
    );
}

int execve(const char* path, char* const argv[], char* const envp[]) {
    return SYSCALL_RET(
        int, syscall3(SYS_EXECVE, (uintptr_t)path, (uintptr_t)argv, (uintptr_t)envp)
    );
}

pid_t getpid(void) {
    return SYSCALL_RET(pid_t, syscall0(SYS_GETPID));
}

pid_t getppid(void) {
    return SYSCALL_RET(pid_t, syscall0(SYS_GETPPID));
}

pid_t getpgid(pid_t pid) {
    return SYSCALL_RET(pid_t, syscall1(SYS_GETPGID, (uintptr_t)pid));
}

int setpgid(pid_t pid, pid_t pgid) {
    return SYSCALL_RET(int, syscall2(SYS_SETPGID, (uintptr_t)pid, (uintptr_t)pgid));
}

pid_t setsid(void) {
    return SYSCALL_RET(pid_t, syscall0(SYS_SETSID));
}

uid_t getuid(void) {
    return SYSCALL_RET(uid_t, syscall0(SYS_GETUID));
}

gid_t getgid(void) {
    return SYSCALL_RET(gid_t, syscall0(SYS_GETGID));
}

int setuid(uid_t uid) {
    return SYSCALL_RET(int, syscall1(SYS_SETUID, (uintptr_t)uid));
}

int setgid(gid_t gid) {
    return SYSCALL_RET(int, syscall1(SYS_SETGID, (uintptr_t)gid));
}

ssize_t getprocs(proc_info_t* out, size_t capacity) {
    return SYSCALL_RET(ssize_t, syscall2(SYS_GETPROCS, (uintptr_t)out, (uintptr_t)capacity));
}

void _exit(int status) {
    syscall1(SYS_EXIT, (uintptr_t)status);

    for (;;)
        ;
}
