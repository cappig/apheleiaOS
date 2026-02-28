#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

struct DIR {
    int fd;
    struct dirent entry;
};

static int _getdents_raw(int fd, struct dirent *out) {
    if (!out) {
        errno = EINVAL;
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        return -1;
    }

    if ((st.st_mode & S_IFMT) != S_IFDIR) {
        errno = ENOTDIR;
        return -1;
    }

    ssize_t ret = read(fd, out, sizeof(*out));
    if (ret <= 0) {
        return (int)ret;
    }

    if ((size_t)ret != sizeof(*out)) {
        errno = EIO;
        return -1;
    }

    return 1;
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
