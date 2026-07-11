#include <dirent.h>
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
    fprintf(stderr, "rm: %s: %s\n", path ? path : "(null)", strerror(errno));
}

static int rm_dir_recursive(const char *path, bool force) {
    DIR *dir = opendir(path);
    if (!dir) {
        if (force && errno == ENOENT) {
            return 0;
        }

        print_error(path);
        return 1;
    }

    int exit_code = 0;
    struct dirent *ent = NULL;
    int read_error = 0;
    for (;;) {
        errno = 0;
        ent = readdir(dir);
        if (!ent) {
            read_error = errno;
            break;
        }

        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
        }

        char child[PATH_MAX];
        if (!fs_join_path(child, sizeof(child), path, ent->d_name)) {
            print_error(path);
            exit_code = 1;
            continue;
        }

        struct stat child_st;
        if (lstat(child, &child_st) < 0) {
            if (!force || errno != ENOENT) {
                print_error(child);
                exit_code = 1;
            }

            continue;
        }

        if (fs_is_dir_mode(child_st.st_mode)) {
            if (rm_dir_recursive(child, force) != 0) {
                exit_code = 1;
            }

            continue;
        }

        if (unlink(child) < 0 && (!force || errno != ENOENT)) {
            print_error(child);
            exit_code = 1;
        }
    }

    if (read_error) {
        errno = read_error;
        print_error(path);
        exit_code = 1;
    }

    if (closedir(dir) < 0) {
        print_error(path);
        exit_code = 1;
    }

    if (rmdir(path) < 0 && (!force || errno != ENOENT)) {
        print_error(path);
        exit_code = 1;
    }

    return exit_code;
}

static int rm_path(const char *path, bool recursive, bool force) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (force && errno == ENOENT) {
            return 0;
        }

        print_error(path);
        return 1;
    }

    if (fs_is_dir_mode(st.st_mode)) {
        if (!recursive) {
            errno = EISDIR;
            print_error(path);
            return 1;
        }

        return rm_dir_recursive(path, force);
    }

    if (unlink(path) < 0) {
        if (force && errno == ENOENT) {
            return 0;
        }

        print_error(path);
        return 1;
    }

    return 0;
}

int main(int argc, char **argv) {
    bool recursive = false;
    bool force = false;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "--")) {
            argi++;
            break;
        }

        const char *opt = argv[argi] + 1;
        if (!opt[0]) {
            io_write_str("usage: rm [-f] [-r] FILE...\n");
            return 1;
        }

        while (*opt) {
            if (*opt == 'f') {
                force = true;
            } else if (*opt == 'r' || *opt == 'R') {
                recursive = true;
            } else {
                io_write_str("usage: rm [-f] [-r] FILE...\n");
                return 1;
            }

            opt++;
        }

        argi++;
    }

    if (argi >= argc) {
        if (force) {
            return 0;
        }

        io_write_str("usage: rm [-f] [-r] FILE...\n");
        return 1;
    }

    int exit_code = 0;
    for (int i = argi; i < argc; i++) {
        if (rm_path(argv[i], recursive, force) != 0) {
            exit_code = 1;
        }
    }

    return exit_code;
}
