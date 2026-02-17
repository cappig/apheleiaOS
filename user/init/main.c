#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

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

static pid_t spawn_getty(const char* tty_path) {
    if (!tty_path || !tty_path[0])
        return -1;

    pid_t pid = fork();

    if (!pid) {
        char* args[] = {"getty", (char*)tty_path, "/bin/login", NULL};

        if (execve("/bin/getty", args, NULL) < 0) {
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
        system("/bin/sh /etc/rc");
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
