#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void write_str(const char* str) {
    if (!str)
        return;

    write(STDOUT_FILENO, str, strlen(str));
}

int main(int argc, char** argv) {
    if (argc < 2) {
        write_str("touch: missing file operand\n");
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_CREAT | O_WRONLY, 0644);
        if (fd < 0) {
            write_str("touch: failed\n");
            status = 1;
            continue;
        }

        close(fd);
    }

    return status;
}
