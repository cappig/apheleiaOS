#include <ctype.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#define INIT_TTY_COUNT 4

typedef struct {
    const char* tty_path;
    pid_t pid;
} getty_slot_t;

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

        if (!count)
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

    if (!pos)
        return -1;

    buf[pos] = '\0';
    return 0;
}

static void run_rc(const char* path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return;

    char line[256];

    while (!read_line_fd(fd, line, sizeof(line))) {
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

static pid_t spawn_getty(const char* tty_path) {
    if (!tty_path || !tty_path[0])
        return -1;

    pid_t pid = fork();

    if (!pid) {
        char* args[] = {"getty", (char*)tty_path, "/sbin/login", NULL};

        if (execve("/sbin/getty", args, NULL) < 0) {
            write_str("init: exec failed\n");
            _exit(1);
        }
    }

    return pid;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (!access("/etc/rc", R_OK)) {
        write_str("init: running /etc/rc\n");
        run_rc("/etc/rc");
    }

    getty_slot_t slots[INIT_TTY_COUNT] = {
        {.tty_path = "/dev/tty0", .pid = -1},
        {.tty_path = "/dev/tty1", .pid = -1},
        {.tty_path = "/dev/tty2", .pid = -1},
        {.tty_path = "/dev/tty3", .pid = -1},
    };

    for (size_t i = 0; i < INIT_TTY_COUNT; i++) {
        slots[i].pid = spawn_getty(slots[i].tty_path);
        if (slots[i].pid < 0) {
            write_str("init: fork failed\n");
            sleep(1);
        }
    }

    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            continue;
        }

        for (size_t i = 0; i < INIT_TTY_COUNT; i++) {
            if (slots[i].pid != pid)
                continue;

            slots[i].pid = spawn_getty(slots[i].tty_path);

            if (slots[i].pid < 0) {
                write_str("init: fork failed\n");
                sleep(1);
            }

            break;
        }
    }

    return 0;
}
