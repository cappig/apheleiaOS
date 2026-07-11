#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int copy_fd(int fd) {
    char buf[BUFSIZ];

    for (;;) {
        ssize_t read_len = read(fd, buf, sizeof(buf));
        if (!read_len) {
            return 0;
        }
        if (read_len < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        size_t off = 0;
        while (off < (size_t)read_len) {
            ssize_t wrote = write(STDOUT_FILENO, buf + off, (size_t)read_len - off);

            if (wrote < 0 && errno == EINTR) {
                continue;
            }
            if (wrote <= 0) {
                if (!wrote) {
                    errno = EIO;
                }
                return -2;
            }

            off += (size_t)wrote;
        }
    }
}

static void print_error(const char *path) {
    fprintf(stderr, "cat: %s: %s\n", path ? path : "standard input", strerror(errno));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        int copy_status = copy_fd(STDIN_FILENO);
        if (copy_status < 0) {
            print_error(copy_status == -2 ? "standard output" : NULL);
            return 1;
        }
        return 0;
    }

    int status = 0;
    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];
        bool use_stdin = !strcmp(path, "-");
        int fd = use_stdin ? STDIN_FILENO : open(path, O_RDONLY);
        if (fd < 0) {
            print_error(path);
            status = 1;
            continue;
        }

        struct stat st = { 0 };
        if (fstat(fd, &st) < 0) {
            print_error(use_stdin ? NULL : path);
            if (!use_stdin) {
                close(fd);
            }
            status = 1;
            continue;
        }

        if ((st.st_mode & S_IFMT) == S_IFDIR) {
            errno = EISDIR;
            print_error(use_stdin ? NULL : path);
            if (!use_stdin) {
                close(fd);
            }
            status = 1;
            continue;
        }

        int copy_status = copy_fd(fd);
        if (copy_status == -2) {
            print_error("standard output");
            if (!use_stdin) {
                close(fd);
            }
            return 1;
        }
        if (copy_status < 0) {
            print_error(use_stdin ? NULL : path);
            status = 1;
        }

        if (!use_stdin && close(fd) < 0) {
            print_error(path);
            status = 1;
        }
    }

    return status;
}
