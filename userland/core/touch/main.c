#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <utime.h>

static void print_error(const char *path) {
    fprintf(stderr, "touch: %s: %s\n", path ? path : "(null)", strerror(errno));
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fputs("touch: missing file operand\n", stderr);
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        if (!utime(argv[i], NULL)) {
            continue;
        }

        if (errno != ENOENT) {
            print_error(argv[i]);
            status = 1;
            continue;
        }

        int fd = open(argv[i], O_CREAT | O_WRONLY, 0666);

        if (fd < 0) {
            print_error(argv[i]);
            status = 1;
            continue;
        }

        if (close(fd) < 0) {
            print_error(argv[i]);
            status = 1;
            continue;
        }

        if (utime(argv[i], NULL) < 0) {
            print_error(argv[i]);
            status = 1;
        }
    }

    return status;
}
