#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void write_str(const char* text) {
    if (!text)
        return;

    write(STDOUT_FILENO, text, strlen(text));
}

static void print_error(const char* path) {
    char line[256];
    snprintf(line, sizeof(line), "rmdir: %s: %d\n", path ? path : "(null)", errno);
    write_str(line);
}

static int remove_with_parents(const char* path, bool parents) {
    if (!path || !path[0]) {
        errno = EINVAL;
        return -1;
    }

    if (rmdir(path) < 0)
        return -1;

    if (!parents)
        return 0;

    char buf[PATH_MAX];
    size_t len = strnlen(path, sizeof(buf));

    if (!len || len >= sizeof(buf))
        return 0;

    memcpy(buf, path, len);
    buf[len] = '\0';

    while (len > 1 && buf[len - 1] == '/') {
        buf[len - 1] = '\0';
        len--;
    }

    while (true) {
        char* slash = strrchr(buf, '/');
        if (!slash)
            break;

        if (slash == buf)
            break;

        *slash = '\0';
        if (buf[0] == '\0')
            break;

        if (rmdir(buf) < 0) {
            if (errno == ENOTEMPTY || errno == ENOENT)
                break;
            return -1;
        }
    }

    return 0;
}

int main(int argc, char** argv) {
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

        write_str("usage: rmdir [-p] DIR...\n");
        return 1;
    }

    if (argi >= argc) {
        write_str("usage: rmdir [-p] DIR...\n");
        return 1;
    }

    int rc = 0;
    for (int i = argi; i < argc; i++) {
        if (remove_with_parents(argv[i], parents) < 0) {
            print_error(argv[i]);
            rc = 1;
        }
    }

    return rc;
}
