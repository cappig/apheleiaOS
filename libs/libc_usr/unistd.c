#include <arch/sys.h>
#include <apheleia/syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <libc_usr/unistd.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/proc.h>
#include <sys/mount.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SYSCALL_RET(type, expr) ((type)__SYSCALL_ERRNO(expr))

ssize_t read(int fd, void *buf, size_t count) {
    return SYSCALL_RET(
        ssize_t,
        syscall3(SYS_READ, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count)
    );
}

ssize_t write(int fd, const void *buf, size_t count) {
    return SYSCALL_RET(
        ssize_t,
        syscall3(SYS_WRITE, (uintptr_t)fd, (uintptr_t)buf, (uintptr_t)count)
    );
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset) {
    return SYSCALL_RET(
        ssize_t,
        syscall4(
            SYS_PREAD,
            (uintptr_t)fd,
            (uintptr_t)buf,
            (uintptr_t)count,
            (uintptr_t)offset
        )
    );
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset) {
    return SYSCALL_RET(
        ssize_t,
        syscall4(
            SYS_PWRITE,
            (uintptr_t)fd,
            (uintptr_t)buf,
            (uintptr_t)count,
            (uintptr_t)offset
        )
    );
}

int open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }

    return SYSCALL_RET(
        int,
        syscall3(SYS_OPEN, (uintptr_t)path, (uintptr_t)flags, (uintptr_t)mode)
    );
}

int close(int fd) {
    return SYSCALL_RET(int, syscall1(SYS_CLOSE, (uintptr_t)fd));
}

int pipe(int pipefd[2]) {
    return SYSCALL_RET(int, syscall1(SYS_PIPE, (uintptr_t)pipefd));
}

int dup(int oldfd) {
    return SYSCALL_RET(int, syscall2(SYS_DUP, (uintptr_t)oldfd, (uintptr_t)-1));
}

int dup2(int oldfd, int newfd) {
    return SYSCALL_RET(
        int, syscall2(SYS_DUP, (uintptr_t)oldfd, (uintptr_t)newfd)
    );
}

int mkdir(const char *path, mode_t mode) {
    return SYSCALL_RET(
        int, syscall2(SYS_MKDIR, (uintptr_t)path, (uintptr_t)mode)
    );
}

int rmdir(const char *path) {
    return SYSCALL_RET(int, syscall1(SYS_RMDIR, (uintptr_t)path));
}

int access(const char *path, int mode) {
    return SYSCALL_RET(
        int, syscall2(SYS_ACCESS, (uintptr_t)path, (uintptr_t)mode)
    );
}

off_t lseek(int fd, off_t offset, int whence) {
    return SYSCALL_RET(
        off_t,
        syscall3(SYS_SEEK, (uintptr_t)fd, (uintptr_t)offset, (uintptr_t)whence)
    );
}

static int _read_proc_value_path(const char *path, long long *out) {
    if (!path || !out) {
        errno = EINVAL;
        return -1;
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }

    char text[32];
    ssize_t n = read(fd, text, sizeof(text) - 1);
    int saved = errno;
    close(fd);

    if (n < 0) {
        errno = saved;
        return -1;
    }

    text[n] = '\0';

    char *end = NULL;
    long long parsed = strtoll(text, &end, 10);
    if (end == text) {
        errno = EINVAL;
        return -1;
    }

    *out = parsed;
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

mode_t umask(mode_t mask) {
    long long old = 0;
    if (_read_proc_value_path("/proc/self/umask", &old) < 0) {
        return (mode_t)-1;
    }

    if (_write_proc_value_path("/proc/self/umask", (long long)(mask & 0777)) < 0) {
        return (mode_t)-1;
    }

    return (mode_t)((unsigned long long)old & 0777ULL);
}

unsigned int sleep(unsigned int seconds) {
    struct timespec req = {
        .tv_sec = (time_t)seconds,
        .tv_nsec = 0,
    };
    struct timespec rem = {0};

    if (nanosleep(&req, &rem) < 0) {
        if (errno == EINTR) {
            unsigned long long left = (unsigned long long)rem.tv_sec;
            if (rem.tv_nsec > 0) {
                left++;
            }
            return left > UINT_MAX ? UINT_MAX : (unsigned int)left;
        }

        return (unsigned int)-1;
    }

    return 0;
}

int chdir(const char *path) {
    return SYSCALL_RET(int, syscall1(SYS_CHDIR, (uintptr_t)path));
}

char *getcwd(char *buf, size_t size) {
    if (!buf || !size) {
        errno = EINVAL;
        return NULL;
    }

    if (size < 2) {
        errno = ERANGE;
        return NULL;
    }

    int fd = open("/proc/self/cwd", O_RDONLY, 0);
    if (fd < 0) {
        return NULL;
    }

    ssize_t n = read(fd, buf, size - 1);
    if (n < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return NULL;
    }

    bool overflow = false;
    if (n == (ssize_t)(size - 1)) {
        char extra = '\0';
        ssize_t extra_n = read(fd, &extra, 1);
        if (extra_n > 0) {
            overflow = true;
        } else if (extra_n < 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return NULL;
        }
    }

    close(fd);

    if (overflow) {
        errno = ERANGE;
        return NULL;
    }

    buf[n] = '\0';
    return buf;
}

int isatty(int fd) {
    termios_t tos;
    return tcgetattr(fd, &tos) ? 0 : 1;
}

int link(const char *oldpath, const char *newpath) {
    return SYSCALL_RET(
        int, syscall2(SYS_LINK, (uintptr_t)oldpath, (uintptr_t)newpath)
    );
}

int unlink(const char *path) {
    return SYSCALL_RET(int, syscall1(SYS_UNLINK, (uintptr_t)path));
}

int rename(const char *oldpath, const char *newpath) {
    return SYSCALL_RET(
        int, syscall2(SYS_RENAME, (uintptr_t)oldpath, (uintptr_t)newpath)
    );
}

int mount(
    const char *source,
    const char *target,
    const char *filesystemtype,
    unsigned long flags
) {
    return SYSCALL_RET(
        int,
        syscall4(
            SYS_MOUNT,
            (uintptr_t)source,
            (uintptr_t)target,
            (uintptr_t)filesystemtype,
            (uintptr_t)flags
        )
    );
}

int umount(const char *target, unsigned long flags) {
    return SYSCALL_RET(
        int,
        syscall2(SYS_UMOUNT, (uintptr_t)target, (uintptr_t)flags)
    );
}

pid_t fork(void) {
    return SYSCALL_RET(pid_t, syscall0(SYS_FORK));
}

pid_t wait(int *status) {
    return waitpid(-1, status, 0);
}

pid_t waitpid(pid_t pid, int *status, int options) {
    return SYSCALL_RET(
        pid_t,
        syscall3(
            SYS_WAITPID, (uintptr_t)pid, (uintptr_t)status, (uintptr_t)options
        )
    );
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return SYSCALL_RET(
        int,
        syscall3(SYS_EXECVE, (uintptr_t)path, (uintptr_t)argv, (uintptr_t)envp)
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
