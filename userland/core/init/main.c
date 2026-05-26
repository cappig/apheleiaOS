#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define INIT_MAX_TTYS     8
#define INIT_TTY_PATH_MAX 64

typedef struct {
    char tty_path[INIT_TTY_PATH_MAX];
    pid_t pid;
    bool optional;
} getty_slot_t;

static void write_str(const char *str) {
    if (!str) {
        return;
    }

    write(STDOUT_FILENO, str, strlen(str));
}

static void set_getty_slot(getty_slot_t *slot, const char *path, bool optional) {
    if (!slot || !path) {
        return;
    }

    strncpy(slot->tty_path, path, sizeof(slot->tty_path) - 1);
    slot->tty_path[sizeof(slot->tty_path) - 1] = '\0';
    slot->pid = -1;
    slot->optional = optional;
}

static size_t default_ttys(getty_slot_t *slots, size_t max_slots) {
    if (!slots || max_slots < 4) {
        return 0;
    }

    set_getty_slot(&slots[0], "/dev/tty0", false);
    set_getty_slot(&slots[1], "/dev/tty1", false);
    set_getty_slot(&slots[2], "/dev/tty2", false);
    set_getty_slot(&slots[3], "/dev/tty3", false);
    return 4;
}

static char *skip_space(char *s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }

    return s;
}

static void trim_line(char *s) {
    size_t len = strlen(s);

    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static size_t parse_ttys(char *buf, getty_slot_t *slots, size_t max_slots) {
    size_t count = 0;
    char *line = buf;

    while (line && count < max_slots) {
        char *next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }

        char *comment = strchr(line, '#');
        if (comment) {
            *comment = '\0';
        }

        line = skip_space(line);
        trim_line(line);

        if (!line[0]) {
            line = next;
            continue;
        }

        bool optional = false;
        char *mode = strchr(line, ' ');
        if (!mode) {
            mode = strchr(line, '\t');
        }

        if (mode) {
            *mode++ = '\0';
            mode = skip_space(mode);
            optional = !strcmp(mode, "optional");
        }

        set_getty_slot(&slots[count++], line, optional);
        line = next;
    }

    return count;
}

static size_t load_ttys(const char *path, getty_slot_t *slots, size_t max_slots) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        return default_ttys(slots, max_slots);
    }

    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        return default_ttys(slots, max_slots);
    }

    buf[n] = '\0';

    size_t count = parse_ttys(buf, slots, max_slots);
    if (!count) {
        count = default_ttys(slots, max_slots);
    }

    return count;
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
        char *args[] = { "sh", (char *)path, NULL };
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

    if (access(tty_path, R_OK | W_OK) < 0) {
        return -1;
    }

    pid_t pid = fork();

    if (!pid) {
        char *args[] = { "getty", (char *)tty_path, "/bin/login", NULL };

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

    getty_slot_t slots[INIT_MAX_TTYS] = { 0 };
    size_t slot_count = load_ttys("/etc/ttys", slots, INIT_MAX_TTYS);

    for (size_t i = 0; i < slot_count; i++) {
        slots[i].pid = spawn_getty(slots[i].tty_path);
        if (slots[i].pid < 0) {
            if (!slots[i].optional) {
                write_str("init: failed to start getty\n");
                sleep(1);
            }
        }
    }

    (void)run_script_sync("/etc/rc");
    run_optional_script_sync("/etc/rc.local");

    for (;;) {
        int status = 0;
        pid_t pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            continue;
        }

        for (size_t i = 0; i < slot_count; i++) {
            if (slots[i].pid != pid) {
                continue;
            }

            slots[i].pid = spawn_getty(slots[i].tty_path);

            if (slots[i].pid < 0) {
                if (!slots[i].optional) {
                    write_str("init: failed to restart getty\n");
                    sleep(1);
                }
            }

            break;
        }
    }

    return 0;
}
