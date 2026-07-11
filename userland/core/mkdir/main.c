#include <errno.h>
#include <fsutil.h>
#include <io.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void print_error(const char *path) {
    fprintf(stderr, "mkdir: %s: %s\n", path ? path : "(null)", strerror(errno));
}

static int ensure_dir(const char *path, mode_t mode) {
    if (!mkdir(path, mode)) {
        return 0;
    }
    if (errno != EEXIST) {
        return -1;
    }

    struct stat st;
    if (stat(path, &st) < 0) {
        return -1;
    }
    if (!fs_is_dir_mode(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

static int mkdir_parents(const char *path, mode_t mode) {
    if (!path || !path[0]) {
        errno = EINVAL;
        return -1;
    }

    char path_copy[PATH_MAX];
    size_t len = strnlen(path, sizeof(path_copy));

    if (!len || len >= sizeof(path_copy)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(path_copy, path, len);
    path_copy[len] = '\0';

    while (len > 1 && path_copy[len - 1] == '/') {
        path_copy[len - 1] = '\0';
        len--;
    }

    char *pos = path_copy;
    if (path_copy[0] == '/') {
        pos++;
    }

    while (*pos) {
        if (*pos != '/') {
            pos++;
            continue;
        }

        if (pos > path_copy && pos[-1] == '/') {
            pos++;
            continue;
        }

        *pos = '\0';

        if (ensure_dir(path_copy, mode) < 0) {
            return -1;
        }

        *pos = '/';
        pos++;
    }

    if (ensure_dir(path_copy, mode) < 0) {
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    bool parents = false;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "--")) {
            argi++;
            break;
        }

        if (!strcmp(argv[argi], "-p")) {
            parents = true;
            argi++;
            continue;
        }

        io_write_str("usage: mkdir [-p] DIR...\n");
        return 1;
    }

    if (argi >= argc) {
        io_write_str("usage: mkdir [-p] DIR...\n");
        return 1;
    }

    int exit_code = 0;
    for (int i = argi; i < argc; i++) {
        int status = parents ? mkdir_parents(argv[i], 0777) : mkdir(argv[i], 0777);

        if (status < 0) {
            print_error(argv[i]);
            exit_code = 1;
        }
    }

    return exit_code;
}
