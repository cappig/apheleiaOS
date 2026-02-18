#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void copy_fd(int fd) {
    char buf[256];

    for (;;) {
        ssize_t read_len = read(fd, buf, sizeof(buf));
        if (read_len <= 0) {
            break;
        }

        size_t off = 0;
        while (off < (size_t)read_len) {
            ssize_t wrote = write(STDOUT_FILENO, buf + off, (size_t)read_len - off);
            if (wrote <= 0) {
                return;
            }
            off += (size_t)wrote;
        }
    }
}

static bool is_dir_fd(int fd) {
    stat_t st = {0};

    if (fstat(fd, &st) < 0) {
        return false;
    }

    return (st.st_mode & S_IFMT) == S_IFDIR;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        copy_fd(STDIN_FILENO);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            write(STDOUT_FILENO, "cat: failed to open\n", 20);
            continue;
        }

        if (is_dir_fd(fd)) {
            char msg[256];
            snprintf(msg, sizeof(msg), "cat: %s: Is a directory\n", argv[i]);
            write(STDOUT_FILENO, msg, strlen(msg));
            close(fd);
            continue;
        }

        copy_fd(fd);
        close(fd);
    }

    return 0;
}
