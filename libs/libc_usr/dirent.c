#include <apheleia/syscall.h>
#include <arch/sys.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

struct DIR {
    int fd;
    struct dirent entry;
};

static int _getdents_raw(int fd, struct dirent *out) {
    return (int)__SYSCALL_ERRNO(
        syscall2(SYS_GETDENTS, (uintptr_t)fd, (uintptr_t)out)
    );
}

#ifdef _APHELEIA_SOURCE
int getdents(int fd, struct dirent *out) {
    return _getdents_raw(fd, out);
}
#endif

DIR *opendir(const char *name) {
    if (!name) {
        errno = EINVAL;
        return NULL;
    }

    int fd = open(name, O_RDONLY);
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
    return dirp;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp) {
        errno = EINVAL;
        return NULL;
    }

    int ret = _getdents_raw(dirp->fd, &dirp->entry);
    if (ret <= 0) {
        return NULL;
    }

    return &dirp->entry;
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
}
