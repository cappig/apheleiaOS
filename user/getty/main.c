#include <fcntl.h>
#include <io.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

static void write_str(const char* str) {
    if (!str)
        return;

    write(STDOUT_FILENO, str, strlen(str));
}

static int attach_tty(const char* path) {
    int fd = open(path, O_RDWR, 0);
    if (fd < 0)
        return -1;

    if (dup(fd, STDIN_FILENO) < 0) {
        close(fd);
        return -1;
    }

    if (dup(fd, STDOUT_FILENO) < 0) {
        close(fd);
        return -1;
    }

    if (dup(fd, STDERR_FILENO) < 0) {
        close(fd);
        return -1;
    }

    if (fd > STDERR_FILENO)
        close(fd);

    return 0;
}

int main(int argc, char** argv) {
    const char* tty_path = "/dev/tty0";
    const char* login_path = "/sbin/login";

    if (argc > 1 && argv[1] && argv[1][0])
        tty_path = argv[1];

    if (argc > 2 && argv[2] && argv[2][0])
        login_path = argv[2];

    if (setsid() < 0)
        return 1;

    if (attach_tty(tty_path) < 0)
        return 1;

    pid_t self = getpid();
    setpgid(0, self);
    ioctl(STDIN_FILENO, TIOCSPGRP, &self);

    char* login_argv[] = {"login", NULL};
    execve(login_path, login_argv, NULL);

    write_str("getty: failed to exec login\n");
    return 1;
}
