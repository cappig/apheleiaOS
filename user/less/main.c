#include "less_doc.h"
#include "less_tty.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    less_doc_t doc = {
        .name = "stdin",
        .data = NULL,
        .len = 0,
        .starts = NULL,
        .count = 0,
    };
    less_tty_t tty = {0};

    int src_fd = STDIN_FILENO;
    int input_fd = STDIN_FILENO;
    bool close_src = false;
    bool close_input = false;
    bool interactive = false;
    int rc = 0;

    if (argc > 2) {
        static const char usage[] = "usage: less [file]\n";
        (void)less_write_all(STDERR_FILENO, usage, sizeof(usage) - 1);
        return 1;
    }

    if (argc == 2) {
        doc.name = argv[1];
        src_fd = open(doc.name, O_RDONLY, 0);
        if (src_fd < 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "less: failed to open %s\n", doc.name);
            (void)less_write_all(STDERR_FILENO, msg, strlen(msg));
            return 1;
        }
        close_src = true;
    }

    if (argc == 1 && isatty(STDIN_FILENO)) {
        static const char usage[] = "usage: less [file]\n";
        (void)less_write_all(STDERR_FILENO, usage, sizeof(usage) - 1);
        return 1;
    }

    if (!less_read_all_fd(src_fd, &doc.data, &doc.len)) {
        static const char msg[] = "less: failed to read input\n";
        (void)less_write_all(STDERR_FILENO, msg, sizeof(msg) - 1);
        rc = 1;
        goto out;
    }

    if (close_src) {
        close(src_fd);
        close_src = false;
        src_fd = -1;
    }

    if (!isatty(STDOUT_FILENO)) {
        if (doc.len) {
            (void)less_write_all(STDOUT_FILENO, doc.data, doc.len);
        }
        goto out;
    }

    if (!isatty(STDIN_FILENO)) {
        if (isatty(STDERR_FILENO)) {
            input_fd = STDERR_FILENO;
        } else if (isatty(STDOUT_FILENO)) {
            input_fd = STDOUT_FILENO;
        } else {
            input_fd = less_open_controlling_input();
            if (input_fd < 0) {
                if (doc.len) {
                    (void)less_write_all(STDOUT_FILENO, doc.data, doc.len);
                }
                goto out;
            }
            close_input = true;
        }
    }

    if (!less_build_line_index(doc.data, doc.len, &doc.starts, &doc.count)) {
        static const char msg[] = "less: out of memory\n";
        (void)less_write_all(STDERR_FILENO, msg, sizeof(msg) - 1);
        rc = 1;
        goto out;
    }

    (void)less_tty_enable_raw(&tty, input_fd);
    interactive = true;

    size_t rows = 24;
    size_t cols = 80;
    size_t top_row = 0;
    bool dirty = true;

    for (;;) {
        bool resized = false;
        less_update_terminal_size(&rows, &cols, input_fd, &resized);
        if (resized) {
            dirty = true;
        }

        if (dirty) {
            if (less_render_page(&doc, rows, cols, &top_row) < 0) {
                rc = 1;
                goto out;
            }
            dirty = false;
        }

        pollfd pfd = {
            .fd = input_fd,
            .events = POLLIN,
            .revents = 0,
        };

        int pr = poll(&pfd, 1, 80);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = 1;
            goto out;
        }

        if (!pr || !(pfd.revents & POLLIN)) {
            continue;
        }

        int key = less_tty_read_key(&tty, input_fd);
        if (key == LESS_KEY_NONE) {
            continue;
        }

        if (key == 'q' || key == 'Q' || key == 3 || key == 27) {
            break;
        }

        size_t page_rows = rows > 1 ? rows - 1 : 1;
        if (
            key == ' ' ||
            key == 'j' ||
            key == 'J' ||
            key == LESS_KEY_PAGE_DOWN ||
            key == LESS_KEY_DOWN
        ) {
            top_row = top_row > SIZE_MAX - page_rows ? SIZE_MAX : top_row + page_rows;
            dirty = true;
        } else if (
            key == 'b' ||
            key == 'k' ||
            key == 'K' ||
            key == LESS_KEY_PAGE_UP ||
            key == LESS_KEY_UP
        ) {
            top_row = top_row > page_rows ? top_row - page_rows : 0;
            dirty = true;
        } else if (key == LESS_KEY_HOME) {
            top_row = 0;
            dirty = true;
        } else if (key == LESS_KEY_END) {
            top_row = SIZE_MAX;
            dirty = true;
        }
    }

out:
    less_tty_restore(&tty);
    if (interactive) {
        (void)less_write_all(STDOUT_FILENO, "\x1b[0m\r\n", 6);
    }
    if (close_src && src_fd >= 0) {
        close(src_fd);
    }
    if (close_input && input_fd >= 0) {
        close(input_fd);
    }
    free(doc.starts);
    free(doc.data);
    return rc;
}
