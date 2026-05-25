#include "term_size.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static bool write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0) {
            return false;
        }

        off += (size_t)n;
    }

    return true;
}

static bool read_byte_default(int fd, char *out, int timeout_ms, void *ctx) {
    (void)ctx;

    if (!out) {
        return false;
    }

    if (timeout_ms >= 0) {
        pollfd pfd = {
            .fd = fd,
            .events = POLLIN,
            .revents = 0,
        };

        for (;;) {
            int rc = poll(&pfd, 1, timeout_ms);
            if (rc < 0 && errno == EINTR) {
                continue;
            }

            if (rc <= 0 || !(pfd.revents & POLLIN)) {
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

static bool size_from_fd(int fd, term_size_t *out) {
    if (fd < 0 || !out) {
        return false;
    }

    winsize_t ws = { 0 };
    if (ioctl(fd, TIOCGWINSZ, &ws)) {
        return false;
    }

    term_size_t size = {
        .rows = ws.ws_row,
        .cols = ws.ws_col,
    };

    if (!term_size_ok(&size)) {
        return false;
    }

    *out = size;
    return true;
}

static bool read_number(int fd, size_t *out, char end_ch, term_read_byte_fn read_byte, void *ctx) {
    if (!out) {
        return false;
    }

    size_t value = 0;

    for (;;) {
        char ch = 0;
        if (!read_byte(fd, &ch, 120, ctx)) {
            return false;
        }

        if (ch == end_ch) {
            *out = value;
            return true;
        }

        if (ch < '0' || ch > '9') {
            return false;
        }

        value = value * 10 + (size_t)(ch - '0');
    }
}

bool term_size_ok(const term_size_t *size) {
    if (!size) {
        return false;
    }

    return (size->cols >= 20 && size->cols <= 512 && size->rows >= 2 && size->rows <= 256);
}

void term_get_size(int input_fd, int output_fd, term_size_t *out, const term_size_t *fallback) {
    if (!out) {
        return;
    }

    if (size_from_fd(output_fd, out)) {
        return;
    }

    if (size_from_fd(input_fd, out)) {
        return;
    }

    if (term_size_ok(fallback)) {
        *out = *fallback;
        return;
    }

    out->rows = 25;
    out->cols = 80;
}

bool term_set_size(int fd, const term_size_t *size) {
    if (fd < 0 || !term_size_ok(size)) {
        return false;
    }

    winsize_t ws = {
        .ws_row = (unsigned short)size->rows,
        .ws_col = (unsigned short)size->cols,
        .ws_xpixel = 0,
        .ws_ypixel = 0,
    };

    return !ioctl(fd, TIOCSWINSZ, &ws);
}

bool term_probe_size(
    int input_fd,
    int output_fd,
    term_size_t *out,
    term_read_byte_fn read_byte,
    term_push_byte_fn push_byte,
    void *ctx
) {
    if (!out) {
        return false;
    }

    if (!read_byte) {
        read_byte = read_byte_default;
    }

    const char query[] = "\x1b"
                         "7\x1b[999;999H\x1b[6n\x1b"
                         "8";
    if (!write_all(output_fd, query, sizeof(query) - 1)) {
        return false;
    }

    char ch = 0;
    if (!read_byte(input_fd, &ch, 1000, ctx)) {
        return false;
    }

    if (ch != '\x1b') {
        if (push_byte) {
            push_byte(input_fd, ch, ctx);
        }

        return false;
    }

    if (!read_byte(input_fd, &ch, 120, ctx) || ch != '[') {
        return false;
    }

    term_size_t size = { 0 };

    if (!read_number(input_fd, &size.rows, ';', read_byte, ctx)) {
        return false;
    }

    if (!read_number(input_fd, &size.cols, 'R', read_byte, ctx)) {
        return false;
    }

    if (!term_size_ok(&size)) {
        return false;
    }

    *out = size;
    (void)term_set_size(input_fd, &size);
    return true;
}
