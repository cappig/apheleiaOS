#include "less_tty.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define LESS_ESC_SEQ_TIMEOUT_MS 10

static bool read_key_byte(int fd, char *out, int timeout_ms) {
    if (fd < 0 || !out) {
        return false;
    }

    if (timeout_ms >= 0) {
        pollfd pfd = {
            .fd = fd,
            .events = POLLIN,
            .revents = 0,
        };

        for (;;) {
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr < 0 && errno == EINTR) {
                continue;
            }
            if (pr <= 0 || !(pfd.revents & POLLIN)) {
                return false;
            }
            break;
        }
    }

    for (;;) {
        ssize_t n = read(fd, out, 1);
        if (n == 1) {
            return true;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

void less_update_terminal_size(
    size_t *rows,
    size_t *cols,
    int input_fd,
    bool *changed
) {
    size_t old_rows = rows ? *rows : 0;
    size_t old_cols = cols ? *cols : 0;
    size_t new_rows = 24;
    size_t new_cols = 80;
    winsize_t ws = {0};

    bool ok = (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_row && ws.ws_col) ||
              (!ioctl(input_fd, TIOCGWINSZ, &ws) && ws.ws_row && ws.ws_col) ||
              (!ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) && ws.ws_row && ws.ws_col);

    if (ok) {
        new_rows = (size_t)ws.ws_row;
        new_cols = (size_t)ws.ws_col;
    }
    if (!new_rows) {
        new_rows = 24;
    }
    if (!new_cols) {
        new_cols = 80;
    }
    if (rows) {
        *rows = new_rows;
    }
    if (cols) {
        *cols = new_cols;
    }
    if (changed) {
        *changed = (old_rows != new_rows) || (old_cols != new_cols);
    }
}

bool less_tty_enable_raw(less_tty_t *tty, int fd) {
    if (!tty || fd < 0 || !isatty(fd)) {
        return false;
    }
    if (tcgetattr(fd, &tty->saved)) {
        return false;
    }

    termios_t raw = tty->saved;
    cfmakeraw(&raw);
    if (tcsetattr(fd, TCSANOW, &raw)) {
        return false;
    }

    tty->saved_fd = fd;
    tty->saved_valid = true;
    tty->raw = true;
    return true;
}

void less_tty_restore(less_tty_t *tty) {
    if (!tty || !tty->saved_valid) {
        return;
    }
    if (tty->saved_fd >= 0) {
        (void)tcsetattr(tty->saved_fd, TCSANOW, &tty->saved);
    }
    tty->saved_fd = -1;
    tty->saved_valid = false;
    tty->raw = false;
}

int less_tty_read_key(const less_tty_t *tty, int fd) {
    char ch = 0;
    if (!read_key_byte(fd, &ch, -1)) {
        return LESS_KEY_NONE;
    }
    if (!tty || !tty->raw || ch != '\x1b') {
        return (unsigned char)ch;
    }

    char a = 0;
    if (!read_key_byte(fd, &a, LESS_ESC_SEQ_TIMEOUT_MS)) {
        return (unsigned char)'\x1b';
    }
    if (a != '[') {
        return (unsigned char)'\x1b';
    }

    char b = 0;
    if (!read_key_byte(fd, &b, LESS_ESC_SEQ_TIMEOUT_MS)) {
        return (unsigned char)'\x1b';
    }
    if (b == 'A') {
        return LESS_KEY_UP;
    }
    if (b == 'B') {
        return LESS_KEY_DOWN;
    }
    if (b == 'H') {
        return LESS_KEY_HOME;
    }
    if (b == 'F') {
        return LESS_KEY_END;
    }
    if (b < '0' || b > '9') {
        return LESS_KEY_NONE;
    }

    int v = b - '0';
    for (;;) {
        char c = 0;
        if (!read_key_byte(fd, &c, LESS_ESC_SEQ_TIMEOUT_MS)) {
            return LESS_KEY_NONE;
        }
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            continue;
        }
        if (c != '~') {
            return LESS_KEY_NONE;
        }
        if (v == 5) {
            return LESS_KEY_PAGE_UP;
        }
        if (v == 6) {
            return LESS_KEY_PAGE_DOWN;
        }
        if (v == 1 || v == 7) {
            return LESS_KEY_HOME;
        }
        if (v == 4 || v == 8) {
            return LESS_KEY_END;
        }
        return LESS_KEY_NONE;
    }
}

int less_open_controlling_input(void) {
    return open("/dev/tty", O_RDONLY, 0);
}
