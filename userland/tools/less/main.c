#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

enum {
    KEY_NONE = -1,
    KEY_UP = 1000,
    KEY_DOWN,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_HOME,
    KEY_END,
};

typedef struct {
    char *data;
    size_t len;
    size_t *line_offsets;
    size_t line_count;
    const char *name;
} doc_t;

typedef struct {
    termios_t saved;
    bool active;
    int fd;
} tty_state_t;

static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static bool read_all_fd(int fd, char **out_data, size_t *out_len) {
    if (!out_data || !out_len) {
        return false;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *data = malloc(cap + 1);
    if (!data) {
        return false;
    }

    for (;;) {
        char chunk[512];
        ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            free(data);
            return false;
        }
        if (got == 0) {
            break;
        }

        size_t need = len + (size_t)got;
        if (need > cap) {
            while (cap < need) {
                cap *= 2;
            }
            char *next = realloc(data, cap + 1);
            if (!next) {
                free(data);
                return false;
            }
            data = next;
        }

        memcpy(data + len, chunk, (size_t)got);
        len += (size_t)got;
    }

    data[len] = '\0';
    *out_data = data;
    *out_len = len;
    return true;
}

static bool build_line_index(doc_t *doc) {
    if (!doc || !doc->data) {
        return false;
    }

    size_t cap = 128;
    size_t *offsets = malloc(cap * sizeof(*offsets));
    if (!offsets) {
        return false;
    }

    size_t count = 0;
    offsets[count++] = 0;

    for (size_t i = 0; i < doc->len; i++) {
        if (doc->data[i] != '\n' || i + 1 >= doc->len) {
            continue;
        }
        if (count >= cap) {
            cap *= 2;
            size_t *next = realloc(offsets, cap * sizeof(*offsets));
            if (!next) {
                free(offsets);
                return false;
            }
            offsets = next;
        }
        offsets[count++] = i + 1;
    }

    doc->line_offsets = offsets;
    doc->line_count = count;
    return true;
}

static size_t line_end(const doc_t *doc, size_t line) {
    size_t start = doc->line_offsets[line];
    size_t end = (line + 1 < doc->line_count) ? doc->line_offsets[line + 1] : doc->len;
    if (end > start && doc->data[end - 1] == '\n') {
        end--;
    }
    return end;
}

static void terminal_size(size_t *rows, size_t *cols, int input_fd) {
    size_t out_rows = 24;
    size_t out_cols = 80;
    winsize_t ws = {0};

    bool ok = (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_row && ws.ws_col) ||
              (!ioctl(input_fd, TIOCGWINSZ, &ws) && ws.ws_row && ws.ws_col);
    if (ok) {
        out_rows = ws.ws_row;
        out_cols = ws.ws_col;
    }

    if (rows) {
        *rows = out_rows;
    }
    if (cols) {
        *cols = out_cols;
    }
}

static bool tty_enter_raw(tty_state_t *tty, int fd) {
    if (!tty || fd < 0 || !isatty(fd)) {
        return false;
    }
    if (tcgetattr(fd, &tty->saved) < 0) {
        return false;
    }

    termios_t raw = tty->saved;
    cfmakeraw(&raw);
    if (tcsetattr(fd, TCSANOW, &raw) < 0) {
        return false;
    }

    tty->active = true;
    tty->fd = fd;
    return true;
}

static void tty_leave_raw(tty_state_t *tty) {
    if (!tty || !tty->active) {
        return;
    }
    (void)tcsetattr(tty->fd, TCSANOW, &tty->saved);
    tty->active = false;
}

static bool read_key_byte(int fd, char *out, int timeout_ms) {
    if (!out) {
        return false;
    }

    if (timeout_ms >= 0) {
        pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
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

static int read_key(int input_fd) {
    char ch = 0;
    if (!read_key_byte(input_fd, &ch, -1)) {
        return KEY_NONE;
    }
    if (ch != '\x1b') {
        return (unsigned char)ch;
    }

    char a = 0;
    if (!read_key_byte(input_fd, &a, 10)) {
        return 27;
    }
    if (a != '[') {
        return 27;
    }

    char b = 0;
    if (!read_key_byte(input_fd, &b, 10)) {
        return 27;
    }

    if (b == 'A') {
        return KEY_UP;
    }
    if (b == 'B') {
        return KEY_DOWN;
    }
    if (b == 'H') {
        return KEY_HOME;
    }
    if (b == 'F') {
        return KEY_END;
    }
    if (b < '0' || b > '9') {
        return KEY_NONE;
    }

    int v = b - '0';
    for (;;) {
        char c = 0;
        if (!read_key_byte(input_fd, &c, 10)) {
            return KEY_NONE;
        }
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            continue;
        }
        if (c != '~') {
            return KEY_NONE;
        }
        if (v == 5) {
            return KEY_PAGE_UP;
        }
        if (v == 6) {
            return KEY_PAGE_DOWN;
        }
        if (v == 1 || v == 7) {
            return KEY_HOME;
        }
        if (v == 4 || v == 8) {
            return KEY_END;
        }
        return KEY_NONE;
    }
}

static int render_page(const doc_t *doc, size_t rows, size_t cols, size_t top_line) {
    size_t page_rows = rows > 1 ? rows - 1 : 1;
    if (write_all(STDOUT_FILENO, "\x1b[H\x1b[2J", 7) < 0) {
        return -1;
    }

    for (size_t i = 0; i < page_rows; i++) {
        size_t line = top_line + i;
        if (line >= doc->line_count) {
            if (write_all(STDOUT_FILENO, "~\x1b[K\r\n", 6) < 0) {
                return -1;
            }
            continue;
        }

        size_t start = doc->line_offsets[line];
        size_t end = line_end(doc, line);
        size_t len = end > start ? (end - start) : 0;
        if (len > cols) {
            len = cols;
        }

        if (len && write_all(STDOUT_FILENO, doc->data + start, len) < 0) {
            return -1;
        }
        if (write_all(STDOUT_FILENO, "\x1b[K\r\n", 5) < 0) {
            return -1;
        }
    }

    char status[192];
    snprintf(
        status,
        sizeof(status),
        "%s  %zu/%zu  (q quit, j/k move, space/b page)",
        doc->name ? doc->name : "stdin",
        top_line + 1,
        doc->line_count
    );

    size_t len = strlen(status);
    if (len > cols) {
        len = cols;
    }
    if (len && write_all(STDOUT_FILENO, status, len) < 0) {
        return -1;
    }
    for (size_t i = len; i < cols; i++) {
        if (write_all(STDOUT_FILENO, " ", 1) < 0) {
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    doc_t doc = {
        .data = NULL,
        .len = 0,
        .line_offsets = NULL,
        .line_count = 0,
        .name = "stdin",
    };
    tty_state_t tty = {.active = false, .fd = -1};

    int src_fd = STDIN_FILENO;
    int input_fd = STDIN_FILENO;
    bool close_src = false;
    bool close_input = false;
    int rc = 0;

    if (argc > 2) {
        (void)write_all(STDERR_FILENO, "usage: less [file]\n", 19);
        return 1;
    }

    if (argc == 2) {
        doc.name = argv[1];
        src_fd = open(doc.name, O_RDONLY, 0);
        if (src_fd < 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "less: failed to open %s\n", doc.name);
            (void)write_all(STDERR_FILENO, msg, strlen(msg));
            return 1;
        }
        close_src = true;
    }

    if (!read_all_fd(src_fd, &doc.data, &doc.len)) {
        (void)write_all(STDERR_FILENO, "less: failed to read input\n", 27);
        rc = 1;
        goto out;
    }

    if (close_src) {
        close(src_fd);
        close_src = false;
    }

    if (!isatty(STDOUT_FILENO)) {
        if (doc.len) {
            (void)write_all(STDOUT_FILENO, doc.data, doc.len);
        }
        goto out;
    }

    if (!build_line_index(&doc)) {
        (void)write_all(STDERR_FILENO, "less: out of memory\n", 20);
        rc = 1;
        goto out;
    }

    bool stdin_is_tty = isatty(STDIN_FILENO);
    bool needs_separate_input = src_fd == STDIN_FILENO;

    if (!stdin_is_tty || needs_separate_input) {
        if (isatty(STDERR_FILENO)) {
            input_fd = STDERR_FILENO;
        } else if (isatty(STDOUT_FILENO)) {
            input_fd = STDOUT_FILENO;
        } else {
            input_fd = open("/dev/tty", O_RDONLY, 0);
            if (input_fd < 0) {
                if (doc.len) {
                    (void)write_all(STDOUT_FILENO, doc.data, doc.len);
                }
                goto out;
            }
            close_input = true;
        }
    }

    if (!tty_enter_raw(&tty, input_fd)) {
        if (doc.len) {
            (void)write_all(STDOUT_FILENO, doc.data, doc.len);
        }
        goto out;
    }

    size_t top_line = 0;
    for (;;) {
        size_t rows = 24;
        size_t cols = 80;
        terminal_size(&rows, &cols, input_fd);

        size_t page = rows > 1 ? rows - 1 : 1;
        size_t max_top = doc.line_count > page ? doc.line_count - page : 0;
        if (top_line > max_top) {
            top_line = max_top;
        }

        if (render_page(&doc, rows, cols, top_line) < 0) {
            rc = 1;
            break;
        }

        int key = read_key(input_fd);
        if (key == KEY_NONE) {
            continue;
        }
        if (key == 'q' || key == 'Q' || key == 3 || key == 27) {
            break;
        }

        if (key == 'g' || key == KEY_HOME) {
            top_line = 0;
            continue;
        }
        if (key == 'G' || key == KEY_END) {
            top_line = max_top;
            continue;
        }

        if (key == 'j' || key == 'J' || key == '\n' || key == KEY_DOWN) {
            if (top_line < max_top) {
                top_line++;
            }
            continue;
        }
        if (key == 'k' || key == 'K' || key == KEY_UP) {
            if (top_line > 0) {
                top_line--;
            }
            continue;
        }

        if (key == ' ' || key == KEY_PAGE_DOWN) {
            size_t next = top_line + page;
            top_line = next > max_top ? max_top : next;
            continue;
        }
        if (key == 'b' || key == KEY_PAGE_UP) {
            top_line = top_line > page ? top_line - page : 0;
            continue;
        }
    }

out:
    tty_leave_raw(&tty);
    if (isatty(STDOUT_FILENO)) {
        (void)write_all(STDOUT_FILENO, "\x1b[0m\r\n", 6);
    }
    if (close_src) {
        close(src_fd);
    }
    if (close_input) {
        close(input_fd);
    }
    free(doc.line_offsets);
    free(doc.data);
    return rc;
}
