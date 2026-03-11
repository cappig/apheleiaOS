#include <errno.h>
#include <io.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void print_error(const char *path) {
    char line[256];
    snprintf(
        line, sizeof(line), "mkdir: %s: %d\n", path ? path : "(null)", errno
    );
    io_write_str(line);
}

static int mkdir_parents(const char *path, mode_t mode) {
    if (!path || !path[0]) {
        errno = EINVAL;
        return -1;
    }

    char tmp[PATH_MAX];
    size_t len = strnlen(path, sizeof(tmp));

    if (!len || len >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(tmp, path, len);
    tmp[len] = '\0';

    while (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
        len--;
    }

    char *pos = tmp;
    if (tmp[0] == '/') {
        pos++;
    }

    while (*pos) {
        if (*pos != '/') {
            pos++;
            continue;
        }

        if (pos > tmp && pos[-1] == '/') {
            pos++;
            continue;
        }

        *pos = '\0';

        if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
            return -1;
        }

        *pos = '/';
        pos++;
    }

    if (mkdir(tmp, mode) < 0 && errno != EEXIST) {
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

    int rc = 0;
    for (int i = argi; i < argc; i++) {
        int ret = parents ? mkdir_parents(argv[i], 0777) : mkdir(argv[i], 0777);

        if (ret < 0) {
            print_error(argv[i]);
            rc = 1;
        }
    }

    return rc;
}
