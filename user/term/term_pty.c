#include "term_pty.h"

#include <errno.h>
#include <fcntl.h>
#include <gui/input.h>
#include <input/kbd.h>
#include <input/keymap.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

static const u8 ctrl_ascii[6] = {'\n', '\n', '\b', '\t', '\e', 0x7f};

static bool write_retry(int fd, const void *data, size_t len) {
    if (fd < 0 || !data || !len) {
        return false;
    }

    const u8 *cursor = data;
    size_t left = len;

    while (left > 0) {
        ssize_t n = write(fd, cursor, left);
        if (n > 0) {
            cursor += (size_t)n;
            left -= (size_t)n;
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }

        return false;
    }

    return true;
}

static void send_bytes(int fd, const char *bytes) {
    if (fd < 0 || !bytes) {
        return;
    }

    write_retry(fd, bytes, strlen(bytes));
}

static bool send_foreground_signal(int master_fd, int signum) {
    if (master_fd < 0 || signum <= 0) {
        return false;
    }

    pid_t pgrp = 0;
    if (ioctl(master_fd, TIOCGPGRP, &pgrp) || pgrp <= 0) {
        return false;
    }

    return kill(-pgrp, signum) >= 0;
}

static char key_to_ascii(u8 code, bool shift) {
    switch (code) {
    case 1 ... 63:
        return shift ? us_keymap.shifted[code] : us_keymap.normal[code];
    case KBD_KP_ENTER ... KBD_DELETE:
        return (char)ctrl_ascii[code - KBD_KP_ENTER];
    default:
        return '\0';
    }
}

void term_handle_key_event(int master_fd, const ws_input_event_t *event) {
    if (!event) {
        return;
    }

    if (!event->action) {
        return;
    }

    bool ctrl = (event->modifiers & INPUT_MOD_CTRL) != 0;
    if (ctrl && event->keycode == KBD_C) {
        if (!send_foreground_signal(master_fd, SIGINT)) {
            const char intr = 0x03;
            write_retry(master_fd, &intr, 1);
        }
        return;
    }

    if (ctrl && event->keycode == KBD_Z) {
        if (!send_foreground_signal(master_fd, SIGTSTP)) {
            const char susp = 0x1a;
            write_retry(master_fd, &susp, 1);
        }
        return;
    }

    switch (event->keycode) {
    case KBD_LEFT:
        send_bytes(master_fd, "\x1b[D");
        return;
    case KBD_RIGHT:
        send_bytes(master_fd, "\x1b[C");
        return;
    case KBD_UP:
        send_bytes(master_fd, "\x1b[A");
        return;
    case KBD_DOWN:
        send_bytes(master_fd, "\x1b[B");
        return;
    case KBD_HOME:
        send_bytes(master_fd, "\x1b[H");
        return;
    case KBD_END:
        send_bytes(master_fd, "\x1b[F");
        return;
    case KBD_DELETE:
        send_bytes(master_fd, "\x1b[3~");
        return;
    default:
        break;
    }

    bool shift = (event->modifiers & INPUT_MOD_SHIFT) != 0;
    bool caps = (event->modifiers & INPUT_MOD_CAPS) != 0;

    char ch = key_to_ascii((u8)event->keycode, shift);
    if (!ch) {
        return;
    }

    if (caps && ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    } else if (caps && ch >= 'A' && ch <= 'Z') {
        ch = (char)(ch - 'A' + 'a');
    }

    if (ctrl) {
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)(ch - 'a' + 1);
        } else if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 1);
        }
    }

    if (ch == '\r') {
        ch = '\n';
    }

    write_retry(master_fd, &ch, 1);
}

pid_t term_spawn_shell(int master_fd, size_t cols, size_t rows, u32 width, u32 height) {
    if (master_fd < 0 || !cols || !rows || !width || !height) {
        return -1;
    }

    winsize_t ws = {
        .ws_col = (u16)cols,
        .ws_row = (u16)rows,
        .ws_xpixel = (u16)width,
        .ws_ypixel = (u16)height,
    };
    ioctl(master_fd, TIOCSWINSZ, &ws);

    int ptn = -1;
    if (ioctl(master_fd, TIOCGPTN, &ptn) || ptn < 0) {
        return -1;
    }

    char path[64];
    snprintf(path, sizeof(path), "/dev/pts%d", ptn);

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (!pid) {
        setsid();
        setpgid(0, 0);

        int slave_fd = open(path, O_RDWR, 0);
        if (slave_fd < 0) {
            _exit(127);
        }

        pid_t pgrp = getpid();
        ioctl(slave_fd, TIOCSPGRP, &pgrp);

        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);

        close(slave_fd);
        close(master_fd);

        char *argv[] = {"/bin/sh", NULL};
        execve("/bin/sh", argv, environ);
        _exit(127);
    }

    return pid;
}
