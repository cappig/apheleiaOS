#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define INIT_TTY_COUNT 4

typedef struct {
    const char *tty_path;
    pid_t pid;
} getty_slot_t;

static void write_str(const char *str) {
    if (!str) {
        return;
    }

    write(STDOUT_FILENO, str, strlen(str));
}

static bool attach_stdio(const char *path) {
    if (!path || !path[0]) {
        return false;
    }

    int fd = open(path, O_RDWR, 0);
    if (fd < 0) {
        return false;
    }

    if (dup2(fd, STDIN_FILENO) < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        if (fd > STDERR_FILENO) {
            close(fd);
        }

        return false;
    }

    if (fd > STDERR_FILENO) {
        close(fd);
    }

    return true;
}

static int run_script_sync(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }

    if (access(path, R_OK) < 0) {
        return 0;
    }

    pid_t pid = fork();
    if (!pid) {
        char *args[] = {"sh", (char *)path, NULL};
        execve("/bin/sh", args, NULL);
        write_str("init: failed to exec startup script\n");
        _exit(127);
    }

    if (pid < 0) {
        write_str("init: failed to fork startup script\n");
        return -1;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }

        write_str("init: waitpid failed for startup script\n");
        return -1;
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code) {
            write_str("init: startup script exited with failure\n");
        }

        return code;
    }

    write_str("init: startup script terminated abnormally\n");
    return -1;
}

static void run_optional_script_sync(const char *path) {
    if (!path || !path[0]) {
        return;
    }

    if (access(path, R_OK) < 0) {
        if (errno != ENOENT) {
            write_str("init: optional startup script not accessible\n");
        }
        return;
    }

    (void)run_script_sync(path);
}

static pid_t spawn_getty(const char *tty_path) {
    if (!tty_path || !tty_path[0]) {
        return -1;
    }

    pid_t pid = fork();

    if (!pid) {
        char *args[] = {"getty", (char *)tty_path, "/bin/login", NULL};

        if (execve("/bin/getty", args, NULL) < 0) {
            write_str("init: exec failed\n");
            _exit(1);
        }
    }

    return pid;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (!attach_stdio("/dev/tty0")) {
        (void)attach_stdio("/dev/console");
    }

    (void)run_script_sync("/etc/rc");
    run_optional_script_sync("/etc/rc.local");

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
            if (slots[i].pid != pid) {
                continue;
            }

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
