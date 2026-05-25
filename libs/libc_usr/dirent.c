#include <apheleia/syscall.h>
#include <arch/sys.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#define DIRENT_BUF_COUNT 8

struct DIR {
    int fd;
    size_t pos;
    size_t count;
    struct dirent entries[DIRENT_BUF_COUNT];
};

static ssize_t _getdents_raw(int fd, struct dirent *out, size_t len) {
    if (!len) {
        return 0;
    }

    if (!out || len < sizeof(*out)) {
        errno = EINVAL;
        return -1;
    }

    return (ssize_t)__SYSCALL_ERRNO(syscall3(SYS_GETDENTS, (uintptr_t)fd, (uintptr_t)out, (uintptr_t)len));
}

#ifdef _APHELEIA_SOURCE
ssize_t getdents(int fd, struct dirent *out, size_t len) {
    return _getdents_raw(fd, out, len);
}
#endif

DIR *opendir(const char *name) {
    if (!name) {
        errno = EINVAL;
        return NULL;
    }

    int fd = open(name, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return NULL;
    }

    DIR *dirp = malloc(sizeof(*dirp));
    if (!dirp) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }

    dirp->fd = fd;
    dirp->pos = 0;
    dirp->count = 0;
    return dirp;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp) {
        errno = EINVAL;
        return NULL;
    }

    if (dirp->pos >= dirp->count) {
        ssize_t ret = _getdents_raw(dirp->fd, dirp->entries, sizeof(dirp->entries));

        if (ret <= 0) {
            return NULL;
        }

        if ((size_t)ret % sizeof(dirp->entries[0])) {
            errno = EIO;
            return NULL;
        }

        dirp->pos = 0;
        dirp->count = (size_t)ret / sizeof(dirp->entries[0]);
    }

    return &dirp->entries[dirp->pos++];
}

int closedir(DIR *dirp) {
    if (!dirp) {
        errno = EINVAL;
        return -1;
    }

    int ret = close(dirp->fd);
    free(dirp);
    return ret;
}

void rewinddir(DIR *dirp) {
    if (!dirp) {
        return;
    }

    (void)lseek(dirp->fd, 0, SEEK_SET);

    dirp->pos = 0;
    dirp->count = 0;
}
