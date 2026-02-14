#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HEAD_BUF_SIZE 256

static int write_all(int fd, const char* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t wrote = write(fd, buf + off, len - off);
        if (wrote <= 0)
            return -1;
        off += (size_t)wrote;
    }
    return 0;
}

static int head_fd(int fd, int lines) {
    char buf[HEAD_BUF_SIZE];
    int remaining = lines;

    while (remaining > 0) {
        ssize_t read_len = read(fd, buf, sizeof(buf));
        if (read_len <= 0)
            break;

        for (ssize_t i = 0; i < read_len && remaining > 0; i++) {
            if (write_all(STDOUT_FILENO, &buf[i], 1) < 0)
                return -1;
            if (buf[i] == '\n')
                remaining--;
        }
    }

    return 0;
}

int main(int argc, char** argv) {
    int lines = 10;
    int argi = 1;

    if (argc >= 3 && !strcmp(argv[1], "-n")) {
        lines = atoi(argv[2]);
        if (lines <= 0)
            lines = 10;
        argi = 3;
    }

    if (argi >= argc) {
        return head_fd(STDIN_FILENO, lines);
    }

    int status = 0;
    for (int i = argi; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            write(STDOUT_FILENO, "head: failed to open\n", 21);
            status = 1;
            continue;
        }

        if (head_fd(fd, lines) < 0)
            status = 1;

        close(fd);
    }

    return status;
}
