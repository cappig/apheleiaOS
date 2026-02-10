#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

static void write_str(const char* str) {
    if (!str)
        return;

    write(STDOUT_FILENO, str, strlen(str));
}

static void strip_newline(char* buf) {
    size_t len = strlen(buf);
    if (!len)
        return;

    if (buf[len - 1] == '\n')
        buf[len - 1] = '\0';
}

static int read_line_fd(int fd, char* buf, size_t len) {
    if (!buf || !len)
        return -1;

    size_t pos = 0;
    bool cr_seen = false;

    while (pos + 1 < len) {
        char ch = 0;
        ssize_t count = read(fd, &ch, 1);
        if (count == 0)
            break;
        if (count < 0)
            continue;

        if (ch == '\r') {
            ch = '\n';
            cr_seen = true;
        } else if (ch == '\n' && cr_seen) {
            cr_seen = false;
            continue;
        } else {
            cr_seen = false;
        }

        buf[pos++] = ch;
        if (ch == '\n')
            break;
    }

    if (pos == 0)
        return -1;

    buf[pos] = '\0';
    return 0;
}

static void run_rc(const char* path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return;

    char line[256];
    while (read_line_fd(fd, line, sizeof(line)) == 0) {
        char* cursor = line;
        while (*cursor && isspace((unsigned char)*cursor))
            cursor++;

        if (!*cursor || *cursor == '#')
            continue;

        strip_newline(cursor);
        system(cursor);
    }

    close(fd);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (access("/etc/rc", R_OK) == 0) {
        write_str("init: running /etc/rc\n");
        run_rc("/etc/rc");
    }

    // write_str("init: starting /sbin/login\n");

    for (;;) {
        pid_t pid = fork();

        if (pid == 0) {
            char* args[] = {"login", NULL};
            if (execve("/sbin/login", args, NULL) < 0) {
                write_str("init: exec failed\n");
                _exit(1);
            }
        }

        if (pid < 0) {
            write_str("init: fork failed\n");
            continue;
        }

        int status = 0;
        wait(pid, &status);
    }

    return 0;
}
