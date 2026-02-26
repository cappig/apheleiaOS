#include "less_doc.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LESS_TAB_WIDTH 8

int less_write_all(int fd, const char *buf, size_t len) {
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

bool less_read_all_fd(int fd, char **out_data, size_t *out_len) {
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
        if (!got) {
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

bool less_build_line_index(
    const char *data,
    size_t len,
    size_t **out_starts,
    size_t *out_count
) {
    if (!out_starts || !out_count) {
        return false;
    }

    size_t *starts = NULL;
    size_t count = 0;
    size_t cap = 128;
    starts = malloc(cap * sizeof(*starts));
    if (!starts) {
        return false;
    }
    starts[count++] = 0;

    for (size_t i = 0; i < len; i++) {
        if (data[i] != '\n' || i + 1 >= len) {
            continue;
        }
        if (count >= cap) {
            size_t next_cap = cap * 2;
            size_t *next = realloc(starts, next_cap * sizeof(*starts));
            if (!next) {
                free(starts);
                return false;
            }
            starts = next;
            cap = next_cap;
        }
        starts[count++] = i + 1;
    }

    *out_starts = starts;
    *out_count = count;
    return true;
}

static size_t _line_end(const less_doc_t *doc, size_t line) {
    size_t end = (line + 1 < doc->count) ? doc->starts[line + 1] : doc->len;
    if (end > doc->starts[line] && doc->data[end - 1] == '\n') {
        end--;
    }
    return end;
}

static size_t _ansi_escape_bytes(const char *s, size_t len, size_t pos) {
    if (!s || pos >= len || (unsigned char)s[pos] != 0x1b) {
        return 0;
    }

    if (pos + 1 >= len || s[pos + 1] != '[') {
        return 1;
    }

    size_t i = pos + 2;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c >= '@' && c <= '~') {
            return i - pos + 1;
        }
        i++;
    }

    return len - pos;
}

static size_t _line_display_cols(const char *s, size_t len) {
    size_t cols = 0;
    size_t pos = 0;

    while (pos < len) {
        size_t esc = _ansi_escape_bytes(s, len, pos);
        if (esc) {
            pos += esc;
            continue;
        }

        unsigned char c = (unsigned char)s[pos];
        if (c == '\t') {
            size_t step = LESS_TAB_WIDTH - (cols % LESS_TAB_WIDTH);
            cols = cols > SIZE_MAX - step ? SIZE_MAX : cols + step;
            pos++;
            continue;
        }

        if (c < 0x20 || c == 0x7f) {
            pos++;
            continue;
        }

        cols = cols == SIZE_MAX ? SIZE_MAX : cols + 1;
        pos++;
    }

    return cols;
}

static size_t _line_rows(const char *s, size_t len, size_t cols) {
    size_t disp = _line_display_cols(s, len);
    return (!cols || !disp) ? 1 : ((disp + cols - 1) / cols);
}

static void _line_render_slice(
    const char *s,
    size_t len,
    size_t start_col,
    size_t width,
    char *out,
    size_t out_cap,
    size_t *out_len
) {
    if (!out_len) {
        return;
    }

    *out_len = 0;
    if (!s || !len || !width || !out || !out_cap) {
        return;
    }

    size_t pos = 0;
    size_t col = 0;
    size_t used = 0;
    size_t end_col = start_col > SIZE_MAX - width ? SIZE_MAX : start_col + width;

    while (pos < len && col < end_col) {
        size_t esc = _ansi_escape_bytes(s, len, pos);
        if (esc) {
            if (used + esc < out_cap) {
                memcpy(out + used, s + pos, esc);
                used += esc;
            }
            pos += esc;
            continue;
        }

        unsigned char c = (unsigned char)s[pos];
        if (c == '\t') {
            size_t tab_w = LESS_TAB_WIDTH - (col % LESS_TAB_WIDTH);
            size_t tab_end = col > SIZE_MAX - tab_w ? SIZE_MAX : col + tab_w;
            size_t paint_from = col > start_col ? col : start_col;
            size_t paint_to = tab_end < end_col ? tab_end : end_col;
            size_t spaces = paint_to > paint_from ? paint_to - paint_from : 0;

            while (spaces--) {
                if (used + 1 >= out_cap) {
                    out[used] = '\0';
                    *out_len = used;
                    return;
                }
                out[used++] = ' ';
            }

            col = tab_end;
            pos++;
            continue;
        }

        if (c < 0x20 || c == 0x7f) {
            pos++;
            continue;
        }

        if (col >= start_col && col < end_col) {
            if (used + 1 >= out_cap) {
                out[used] = '\0';
                *out_len = used;
                return;
            }
            out[used++] = s[pos];
        }

        col = col == SIZE_MAX ? SIZE_MAX : col + 1;
        pos++;
    }

    if (used < out_cap) {
        out[used] = '\0';
    }
    *out_len = used;
}

static void _scan_to_row(
    const less_doc_t *doc,
    size_t cols,
    size_t row,
    size_t *out_line,
    size_t *out_off,
    size_t *out_total
) {
    size_t total = 0;
    size_t line = doc ? doc->count : 0;
    size_t off = 0;
    bool found = false;

    for (size_t i = 0; doc && doc->starts && i < doc->count; i++) {
        size_t start = doc->starts[i];
        size_t end = _line_end(doc, i);
        size_t len = end >= start ? (end - start) : 0;
        const char *line_ptr = doc->data + start;
        size_t rows = _line_rows(line_ptr, len, cols);
        size_t next_total = (rows > SIZE_MAX - total) ? SIZE_MAX : total + rows;

        if (!found && row < next_total) {
            found = true;
            line = i;
            off = (row - total) * cols;
        }
        total = next_total;
    }

    if (!total) {
        total = 1;
    }
    if (!found) {
        line = doc ? doc->count : 0;
        off = 0;
    }

    if (out_line) {
        *out_line = line;
    }
    if (out_off) {
        *out_off = off;
    }
    if (out_total) {
        *out_total = total;
    }
}

int less_render_page(
    const less_doc_t *doc,
    size_t rows,
    size_t cols,
    size_t *top_row
) {
    if (!doc || !doc->data || !doc->starts || !doc->count || !rows || !cols || !top_row) {
        return -1;
    }

    size_t page_rows = rows > 1 ? rows - 1 : 1;
    size_t req = *top_row;
    size_t line = doc->count;
    size_t off = 0;
    size_t total = 1;

    if (req == SIZE_MAX) {
        _scan_to_row(doc, cols, SIZE_MAX - 1, &line, &off, &total);
        req = total > page_rows ? total - page_rows : 0;
    }

    _scan_to_row(doc, cols, req, &line, &off, &total);
    size_t max_top = total > page_rows ? total - page_rows : 0;
    if (req > max_top) {
        req = max_top;
        _scan_to_row(doc, cols, req, &line, &off, &total);
    }
    *top_row = req;

    if (less_write_all(STDOUT_FILENO, "\x1b[H\x1b[2J", 7) < 0) {
        return -1;
    }

    size_t row_buf_cap = cols * 16 + 256;
    char *row_buf = malloc(row_buf_cap);
    if (!row_buf) {
        return -1;
    }

    for (size_t row = 0; row < page_rows; row++) {
        if (line >= doc->count) {
            if (less_write_all(STDOUT_FILENO, "~\x1b[0m\x1b[K\r\n", 10) < 0) {
                free(row_buf);
                return -1;
            }
            continue;
        }

        size_t start = doc->starts[line];
        size_t end = _line_end(doc, line);
        size_t len = end >= start ? (end - start) : 0;
        const char *line_ptr = doc->data + start;
        size_t line_cols = _line_display_cols(line_ptr, len);
        size_t show_len = 0;

        if (off < line_cols) {
            _line_render_slice(
                line_ptr,
                len,
                off,
                cols,
                row_buf,
                row_buf_cap,
                &show_len
            );
        }
        if (show_len && less_write_all(STDOUT_FILENO, row_buf, show_len) < 0) {
            free(row_buf);
            return -1;
        }
        if (less_write_all(STDOUT_FILENO, "\x1b[0m\x1b[K\r\n", 9) < 0) {
            free(row_buf);
            return -1;
        }

        if (!line_cols || off + cols >= line_cols) {
            line++;
            off = 0;
        } else {
            off += cols;
        }
    }

    size_t last = req + page_rows;
    if (last > total) {
        last = total;
    }
    size_t pct = total ? (last * 100) / total : 100;

    char status[256];
    snprintf(
        status,
        sizeof(status),
        "%s  rows %zu-%zu/%zu  %zu%%  (q/esc quit, j next page, k prev page)",
        doc->name ? doc->name : "stdin",
        req + 1,
        last,
        total,
        pct
    );

    size_t slen = strlen(status);
    size_t show = slen < cols ? slen : cols;
    if (show && less_write_all(STDOUT_FILENO, status, show) < 0) {
        free(row_buf);
        return -1;
    }
    for (size_t i = show; i < cols; i++) {
        if (less_write_all(STDOUT_FILENO, " ", 1) < 0) {
            free(row_buf);
            return -1;
        }
    }
    free(row_buf);
    return 0;
}
