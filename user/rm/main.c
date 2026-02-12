#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_str(const char* text) {
    if (!text)
        return;

    write(STDOUT_FILENO, text, strlen(text));
}

static void print_error(const char* path) {
    char line[256];
    snprintf(line, sizeof(line), "rm: %s: %d\n", path ? path : "(null)", errno);
    write_str(line);
}

static bool is_dir_mode(mode_t mode) {
    return (mode & S_IFMT) == S_IFDIR;
}

static void join_path(char* out, size_t out_len, const char* left, const char* right) {
    if (!out || out_len == 0)
        return;

    if (!left || !left[0]) {
        snprintf(out, out_len, "%s", right ? right : "");
        return;
    }

    size_t len = strlen(left);
    if (len > 0 && left[len - 1] == '/')
        snprintf(out, out_len, "%s%s", left, right ? right : "");
    else
        snprintf(out, out_len, "%s/%s", left, right ? right : "");
}

static int rm_path(const char* path, bool recursive, bool force);

static int rm_dir_recursive(const char* path, bool force) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        if (force)
            return 0;
        print_error(path);
        return 1;
    }

    int rc = 0;
    dirent_t ent;

    while (getdents(fd, &ent) > 0) {
        if (!strcmp(ent.d_name, ".") || !strcmp(ent.d_name, ".."))
            continue;

        char child[PATH_MAX];
        join_path(child, sizeof(child), path, ent.d_name);
        if (rm_path(child, true, force) != 0)
            rc = 1;
    }

    close(fd);

    if (rmdir(path) < 0) {
        if (!force) {
            print_error(path);
            rc = 1;
        }
    }

    return rc;
}

static int rm_path(const char* path, bool recursive, bool force) {
    stat_t st;
    if (lstat(path, &st) < 0) {
        if (force && errno == ENOENT)
            return 0;
        print_error(path);
        return 1;
    }

    if (is_dir_mode(st.st_mode)) {
        if (!recursive) {
            errno = EISDIR;
            print_error(path);
            return 1;
        }

        return rm_dir_recursive(path, force);
    }

    if (unlink(path) < 0) {
        if (force && errno == ENOENT)
            return 0;
        print_error(path);
        return 1;
    }

    return 0;
}

int main(int argc, char** argv) {
    bool recursive = false;
    bool force = false;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "--")) {
            argi++;
            break;
        }

        const char* opt = argv[argi] + 1;
        if (!opt[0]) {
            write_str("usage: rm [-f] [-r] FILE...\n");
            return 1;
        }

        while (*opt) {
            if (*opt == 'f')
                force = true;
            else if (*opt == 'r' || *opt == 'R')
                recursive = true;
            else {
                write_str("usage: rm [-f] [-r] FILE...\n");
                return 1;
            }
            opt++;
        }

        argi++;
    }

    if (argi >= argc) {
        if (force)
            return 0;
        write_str("usage: rm [-f] [-r] FILE...\n");
        return 1;
    }

    int rc = 0;
    for (int i = argi; i < argc; i++) {
        if (rm_path(argv[i], recursive, force) != 0)
            rc = 1;
    }

    return rc;
}
