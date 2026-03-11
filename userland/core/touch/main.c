#include <fcntl.h>
#include <io.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        io_write_str("touch: missing file operand\n");
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_CREAT | O_WRONLY, 0644);

        if (fd < 0) {
            io_write_str("touch: failed\n");
            status = 1;
            continue;
        }

        close(fd);
    }

    return status;
}
