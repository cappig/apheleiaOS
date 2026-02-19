#include <arch/sys.h>
#include <errno.h>
#include <fcntl.h>
#include <libc_usr/unistd.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/proc.h>
#include <termios.h>
#include <unistd.h>

#define SYSCALL_RET(type, expr) ((type)__SYSCALL_ERRNO(expr))

ssize_t read(int fd, void *buf, size_t count) {
    return SYSCALL_RET(
        ssize_t, syscall3(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count)
    );
}

ssize_t write(int fd, const void *buf, size_t count) {
    return SYSCALL_RET(
        ssize_t, syscall3(SYS_WRITE, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count)
    );
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    return SYSCALL_RET(
        ssize_t,
        syscall4(SYS_PREAD, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count, (uintptr_t)offset)
    );
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    return SYSCALL_RET(
        ssize_t,
        syscall4(SYS_PWRITE, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count, (uintptr_t)offset)
    );
}

int open(const char *path, int flags, mode_t mode) {
    return SYSCALL_RET(int, syscall3(SYS_OPEN, (uintptr_t)path, (uintptr_t)flags, (uintptr_t)mode));
}

int close(int fd) {
    return SYSCALL_RET(int, syscall1(SYS_CLOSE, (uintptr_t)fd));
}

int pipe(int pipefd[2]) {
    return SYSCALL_RET(int, syscall1(SYS_PIPE, (uintptr_t)pipefd));
}

int dup(int oldfd, int newfd) {
    return SYSCALL_RET(int, syscall2(SYS_DUP, (uintptr_t)oldfd, (uintptr_t)newfd));
}

int mkdir(const char *path, mode_t mode) {
    return SYSCALL_RET(int, syscall2(SYS_MKDIR, (uintptr_t)path, (uintptr_t)mode));
}

int rmdir(const char *path) {
    return SYSCALL_RET(int, syscall1(SYS_RMDIR, (uintptr_t)path));
}

int access(const char *path, int mode) {
    return SYSCALL_RET(int, syscall2(SYS_ACCESS, (uintptr_t)path, (uintptr_t)mode));
}

off_t lseek(int fd, off_t offset, int whence) {
    return SYSCALL_RET(
        off_t, syscall3(SYS_SEEK, (uintptr_t)fd, (uintptr_t)offset, (uintptr_t)whence)
    );
}

mode_t umask(mode_t mask) {
    return SYSCALL_RET(mode_t, syscall1(SYS_UMASK, (uintptr_t)mask));
}

unsigned int sleep(unsigned int seconds) {
    unsigned long long total_ms = (unsigned long long)seconds * 1000ULL;
    unsigned int ms = total_ms > UINT_MAX ? UINT_MAX : (unsigned int)total_ms;

    long ret = syscall1(SYS_SLEEP, (uintptr_t)ms);
    if (ret < 0) {
        errno = (int)-ret;
        return (unsigned int)-1;
    }
    return (unsigned int)ret;
}

int chdir(const char *path) {
    return SYSCALL_RET(int, syscall1(SYS_CHDIR, (uintptr_t)path));
}

char *getcwd(char *buf, size_t size) {
    if (!buf || !size) {
        return NULL;
    }

    long ret = __SYSCALL_ERRNO(syscall2(SYS_GETCWD, (uintptr_t)buf, (uintptr_t)size));
    return !ret ? buf : NULL;
}

int isatty(int fd) {
    termios_t tos;
    return tcgetattr(fd, &tos) ? 0 : 1;
}

int link(const char *oldpath, const char *newpath) {
    return SYSCALL_RET(int, syscall2(SYS_LINK, (uintptr_t)oldpath, (uintptr_t)newpath));
}

int unlink(const char *path) {
    return SYSCALL_RET(int, syscall1(SYS_UNLINK, (uintptr_t)path));
}

int rename(const char *oldpath, const char *newpath) {
    return SYSCALL_RET(int, syscall2(SYS_RENAME, (uintptr_t)oldpath, (uintptr_t)newpath));
}

pid_t fork(void) {
    return SYSCALL_RET(pid_t, syscall0(SYS_FORK));
}

pid_t wait(pid_t pid, int *status) {
    return SYSCALL_RET(pid_t, syscall2(SYS_WAIT, (uintptr_t)pid, (uintptr_t)status));
}

pid_t waitpid(pid_t pid, int *status, int options) {
    return SYSCALL_RET(
        pid_t, syscall3(SYS_WAITPID, (uintptr_t)pid, (uintptr_t)status, (uintptr_t)options)
    );
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return SYSCALL_RET(
        int, syscall3(SYS_EXECVE, (uintptr_t)path, (uintptr_t)argv, (uintptr_t)envp)
    );
}

static int _read_self_stat(proc_stat_t *stat_out) {
    if (!stat_out) {
        errno = EINVAL;
        return -1;
    }

    if (proc_stat_read_path("/proc/self/stat", stat_out) < 0) {
        return -1;
    }

    return 0;
}

static int _write_proc_value_path(const char *path, long long value) {
    if (!path) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, O_WRONLY, 0);
    if (fd < 0) {
        return -1;
    }

    char text[32];
    int len = snprintf(text, sizeof(text), "%lld\n", value);
    if (len <= 0 || (size_t)len >= sizeof(text)) {
        close(fd);
        errno = EINVAL;
        return -1;
    }

    ssize_t written = write(fd, text, (size_t)len);
    int saved = errno;

    close(fd);

    if (written < 0) {
        errno = saved;
        return -1;
    }

    if ((size_t)written != (size_t)len) {
        errno = EIO;
        return -1;
    }

    return 0;
}

pid_t getpid(void) {
    proc_stat_t stat = {0};
    if (_read_self_stat(&stat) < 0) {
        return (pid_t)-1;
    }

    return stat.pid;
}

pid_t getppid(void) {
    proc_stat_t stat = {0};
    if (_read_self_stat(&stat) < 0) {
        return (pid_t)-1;
    }

    return stat.ppid;
}

pid_t getpgid(pid_t pid) {
    if (pid < 0) {
        errno = EINVAL;
        return -1;
    }

    proc_stat_t stat = {0};

    if (!pid) {
        if (_read_self_stat(&stat) < 0) {
            return -1;
        }

        return stat.pgid;
    }

    if (proc_stat_read(pid, &stat) < 0) {
        return -1;
    }

    return stat.pgid;
}

int setpgid(pid_t pid, pid_t pgid) {
    if (pid < 0 || pgid < 0) {
        errno = EINVAL;
        return -1;
    }

    if (!pid) {
        return _write_proc_value_path("/proc/self/pgid", (long long)pgid);
    }

    char path[48];
    snprintf(path, sizeof(path), "/proc/%lld/pgid", (long long)pid);
    return _write_proc_value_path(path, (long long)pgid);
}

pid_t setsid(void) {
    if (_write_proc_value_path("/proc/self/sid", 1) < 0) {
        return -1;
    }

    proc_stat_t stat = {0};
    if (_read_self_stat(&stat) < 0) {
        return -1;
    }

    return stat.sid;
}

uid_t getuid(void) {
    proc_stat_t stat = {0};
    if (_read_self_stat(&stat) < 0) {
        return (uid_t)-1;
    }

    return stat.uid;
}

gid_t getgid(void) {
    proc_stat_t stat = {0};
    if (_read_self_stat(&stat) < 0) {
        return (gid_t)-1;
    }

    return stat.gid;
}

int setuid(uid_t uid) {
    return _write_proc_value_path("/proc/self/uid", (long long)uid);
}

int setgid(gid_t gid) {
    return _write_proc_value_path("/proc/self/gid", (long long)gid);
}

void _exit(int status) {
    syscall1(SYS_EXIT, (uintptr_t)status);

    for (;;) {
        ;
    }
}
