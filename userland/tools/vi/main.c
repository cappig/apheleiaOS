#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <term_size.h>
#include <termios.h>
#include <unistd.h>

#define CTRL(k) ((k) & 0x1f)

#define VI_INIT_CAP 4096
#define VI_CMD_MAX  64
#define VI_MSG_MAX  128
#define VI_OUT_INIT 8192
#define VI_TAB_STOP 8

enum vi_key {
    VI_KEY_NONE = 0,
    VI_KEY_UP = 1000,
    VI_KEY_DOWN,
    VI_KEY_LEFT,
    VI_KEY_RIGHT,
    VI_KEY_DEL,
    VI_KEY_HOME,
    VI_KEY_END,
    VI_KEY_RESIZE,
};

typedef enum {
    VI_NORMAL = 0,
    VI_INSERT = 1,
    VI_COMMAND = 2,
} vi_mode_t;

typedef struct {
    const char *path;
    char path_buf[128];

    char *buf;
    size_t len;
    size_t cap;
    size_t cursor;
    bool dirty;

    size_t cols;
    size_t edit_rows;
    size_t rowoff;
    size_t coloff;

    vi_mode_t mode;
    bool running;
    bool redraw;
    bool repaint;

    char msg[VI_MSG_MAX];
    char cmd[VI_CMD_MAX];
    size_t cmd_len;
    char pending_op;

    termios_t saved_tty;
    bool saved_tty_valid;
} vi_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} vi_out_t;

typedef struct {
    vi_t editor;
    vi_out_t out;

    volatile sig_atomic_t got_sigwinch;

    char *frame_cache;
    size_t frame_cache_cols;
    size_t frame_cache_rows;

    char *row_scratch;
    size_t row_scratch_cap;

    char key_push;
    bool key_push_valid;
} vi_app_t;

static vi_app_t app = { 0 };

static void raw_mode_off(void) {
    if (!app.editor.saved_tty_valid) {
        return;
    }

    ioctl(STDIN_FILENO, TCSETS, &app.editor.saved_tty);
}

static bool write_all_fd(int fd, const char *buf, size_t len) {
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

static void vi_write_literal(const char *text) {
    if (!text) {
        return;
    }

    (void)write_all_fd(STDERR_FILENO, text, strlen(text));
}

static void fatal_signal_handler(int signum) {
    raw_mode_off();
    vi_write_literal("\x1b[?25h\x1b[0m\r\nvi: fatal signal ");

    char num[16];
    snprintf(num, sizeof(num), "%d", signum);
    vi_write_literal(num);
    vi_write_literal("\r\n");

    _exit(128 + signum);
}

static bool ensure_row_scratch(size_t cols) {
    size_t need = cols + 1;
    if (app.row_scratch_cap >= need) {
        return true;
    }

    char *p = realloc(app.row_scratch, need);
    if (!p) {
        return false;
    }

    app.row_scratch = p;
    app.row_scratch_cap = need;
    return true;
}

static bool ensure_frame_cache(size_t cols, size_t rows, bool *resized_out) {
    if (resized_out) {
        *resized_out = false;
    }

    if (app.frame_cache && app.frame_cache_cols == cols && app.frame_cache_rows == rows) {
        return true;
    }

    // cached cells let the renderer skip unchanged screen positions on slow serial consoles
    if (!cols || !rows) {
        free(app.frame_cache);

        app.frame_cache = NULL;
        app.frame_cache_cols = 0;
        app.frame_cache_rows = 0;

        if (resized_out) {
            *resized_out = true;
        }

        return true;
    }

    size_t total = cols * rows;
    char *p = realloc(app.frame_cache, total);
    if (!p) {
        return false;
    }

    app.frame_cache = p;
    app.frame_cache_cols = cols;
    app.frame_cache_rows = rows;

    memset(app.frame_cache, 0xff, total);

    if (resized_out) {
        *resized_out = true;
    }

    app.editor.repaint = true;
    return true;
}

static bool out_addn(const char *text, size_t n) {
    if (!text || !n) {
        return true;
    }

    if (app.out.len + n > app.out.cap) {
        size_t need = app.out.len + n;
        size_t cap = app.out.cap ? app.out.cap : VI_OUT_INIT;

        while (cap < need) {
            cap *= 2;
        }

        char *p = realloc(app.out.data, cap);
        if (!p) {
            return false;
        }

        app.out.data = p;
        app.out.cap = cap;
    }

    memcpy(app.out.data + app.out.len, text, n);
    app.out.len += n;
    return true;
}

static bool out_add(const char *text) {
    if (!text) {
        return true;
    }

    return out_addn(text, strlen(text));
}

static bool draw_row_if_changed(size_t row, const char *data) {
    if (!data || !app.frame_cache || row >= app.frame_cache_rows || app.editor.cols != app.frame_cache_cols) {
        return false;
    }

    char *cached = app.frame_cache + (row * app.frame_cache_cols);
    if (!app.editor.repaint && !memcmp(cached, data, app.editor.cols)) {
        return true;
    }

    char seq[32];
    snprintf(seq, sizeof(seq), "\x1b[%u;1H", (unsigned)(row + 1));

    size_t draw_cols = app.editor.cols;
    if (draw_cols > 1) {
        draw_cols--;
    }

    if (!out_add(seq) || !out_addn(data, draw_cols) || !out_add("\x1b[K")) {
        return false;
    }

    memcpy(cached, data, app.editor.cols);
    return true;
}

static bool out_flush(void) {
    return write_all_fd(STDOUT_FILENO, app.out.data, app.out.len);
}

static void set_msg(const char *text) {
    if (!text) {
        return;
    }

    snprintf(app.editor.msg, sizeof(app.editor.msg), "%s", text);
}

static void clear_msg(void) {
    app.editor.msg[0] = '\0';
}

static void set_errno_msg(const char *op) {
    char msg[VI_MSG_MAX];
    snprintf(msg, sizeof(msg), "%s failed: %s", op, strerror(errno));
    set_msg(msg);
}

static bool is_nav_key(int key) {
    return (
        key == VI_KEY_LEFT || key == VI_KEY_RIGHT || key == VI_KEY_UP || key == VI_KEY_DOWN || key == VI_KEY_HOME ||
        key == VI_KEY_END
    );
}

static void clear_cmd(void) {
    app.editor.cmd_len = 0;
    app.editor.cmd[0] = '\0';
}

static void quit_vi(void) {
    app.editor.running = false;
}

static void screen_enter(void) {
    const char seq[] = "\r\x1b[0m\x1b[2K"
                       "\x1b[?1049h"
                       "\x1b[?25l\x1b[H\x1b[2J\x1b[3J\x1b[H";

    (void)write_all_fd(STDOUT_FILENO, seq, sizeof(seq) - 1);
    app.editor.repaint = true;
}

static void screen_leave(void) {
    const char seq[] = "\x1b[?25h\x1b[0m\x1b[?1049l\r\x1b[K";
    (void)write_all_fd(STDOUT_FILENO, seq, sizeof(seq) - 1);
}

static void enter_mode(vi_mode_t mode) {
    app.editor.pending_op = 0;
    app.editor.mode = mode;
    app.editor.redraw = true;
}

static size_t visual_advance(size_t col, char ch) {
    if (ch == '\t') {
        return ((col / VI_TAB_STOP) + 1) * VI_TAB_STOP;
    }

    return col + 1;
}

static void update_screen_size(void) {
    const term_size_t fallback = {
        .rows = 25,
        .cols = 80,
    };

    term_size_t size = fallback;
    term_get_size(STDIN_FILENO, STDOUT_FILENO, &size, &fallback);

    app.editor.cols = size.cols;
    app.editor.edit_rows = size.rows > 1 ? size.rows - 1 : 1;
}

static bool raw_mode_on(void) {
    termios_t tos = { 0 };
    if (ioctl(STDIN_FILENO, TCGETS, &tos)) {
        return false;
    }

    app.editor.saved_tty = tos;
    app.editor.saved_tty_valid = true;

    tos.c_iflag &= (tcflag_t) ~(ICRNL | IXON | BRKINT | INPCK | ISTRIP);
    tos.c_oflag &= (tcflag_t) ~(OPOST);
    tos.c_cflag |= (tcflag_t)CS8;
    tos.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    tos.c_cc[VMIN] = 1;
    tos.c_cc[VTIME] = 0;

    return !ioctl(STDIN_FILENO, TCSETS, &tos);
}

static void key_push_back(char ch) {
    app.key_push = ch;
    app.key_push_valid = true;
}

static bool read_key_byte(char *out_ch, int timeout_ms) {
    if (!out_ch) {
        return false;
    }

    if (app.key_push_valid) {
        app.key_push_valid = false;
        *out_ch = app.key_push;
        return true;
    }

    if (timeout_ms >= 0) {
        pollfd pfd = {
            .fd = STDIN_FILENO,
            .events = POLLIN,
            .revents = 0,
        };

        for (;;) {
            int rc = poll(&pfd, 1, timeout_ms);
            if (rc < 0 && errno == EINTR) {
                if (app.got_sigwinch) {
                    return false;
                }

                continue;
            }

            if (rc <= 0 || !(pfd.revents & POLLIN)) {
                return false;
            }

            break;
        }
    }

    for (;;) {
        ssize_t n = read(STDIN_FILENO, out_ch, 1);
        if (n == 1) {
            return true;
        }

        if (n < 0 && errno == EINTR) {
            if (app.got_sigwinch) {
                return false;
            }

            continue;
        }

        return false;
    }
}

static bool read_term_byte(int fd, char *out_ch, int timeout_ms, void *ctx) {
    (void)fd;
    (void)ctx;

    return read_key_byte(out_ch, timeout_ms);
}

static void push_term_byte(int fd, char ch, void *ctx) {
    (void)fd;
    (void)ctx;

    key_push_back(ch);
}

static int parse_csi_key(void) {
    char seq[16] = { 0 };
    size_t len = 0;

    while (len + 1 < sizeof(seq)) {
        char ch = 0;
        if (!read_key_byte(&ch, 80)) {
            return VI_KEY_NONE;
        }

        seq[len++] = ch;

        if (ch >= '@' && ch <= '~') {
            break;
        }
    }

    char final = seq[len ? len - 1 : 0];

    if (len == 1) {
        switch (final) {
        case 'A':
            return VI_KEY_UP;
        case 'B':
            return VI_KEY_DOWN;
        case 'C':
            return VI_KEY_RIGHT;
        case 'D':
            return VI_KEY_LEFT;
        case 'H':
            return VI_KEY_HOME;
        case 'F':
            return VI_KEY_END;
        default:
            return VI_KEY_NONE;
        }
    }

    if (final == 'R') {
        return VI_KEY_NONE;
    }

    if (final != '~') {
        return VI_KEY_NONE;
    }

    int value = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        if (seq[i] < '0' || seq[i] > '9') {
            return VI_KEY_NONE;
        }

        value = value * 10 + (seq[i] - '0');
    }

    switch (value) {
    case 1:
    case 7:
        return VI_KEY_HOME;
    case 3:
        return VI_KEY_DEL;
    case 4:
    case 8:
        return VI_KEY_END;
    default:
        return VI_KEY_NONE;
    }
}

static bool detect_screen_size(void) {
    term_size_t size = { 0 };

    bool probed = term_probe_size(STDIN_FILENO, STDOUT_FILENO, &size, read_term_byte, push_term_byte, NULL);
    if (!probed) {
        return false;
    }

    app.editor.cols = size.cols;
    app.editor.edit_rows = size.rows - 1;
    app.editor.repaint = true;
    app.editor.redraw = true;
    return true;
}

static int read_key(void) {
    if (app.got_sigwinch) {
        app.got_sigwinch = 0;
        return VI_KEY_RESIZE;
    }

    char ch = 0;
    if (!read_key_byte(&ch, -1)) {
        if (app.got_sigwinch) {
            app.got_sigwinch = 0;
            return VI_KEY_RESIZE;
        }

        return VI_KEY_NONE;
    }

    if (ch != '\x1b') {
        return (unsigned char)ch;
    }

    char intro = 0;
    if (!read_key_byte(&intro, 80)) {
        return '\x1b';
    }

    if (intro == '[') {
        return parse_csi_key();
    }

    if (intro == 'O') {
        char final = 0;
        if (!read_key_byte(&final, 80)) {
            return VI_KEY_NONE;
        }

        if (final == 'H') {
            return VI_KEY_HOME;
        }

        if (final == 'F') {
            return VI_KEY_END;
        }

        return VI_KEY_NONE;
    }

    key_push_back(intro);
    return '\x1b';
}

static size_t line_start(size_t idx) {
    if (idx > app.editor.len) {
        idx = app.editor.len;
    }

    while (idx > 0 && app.editor.buf[idx - 1] != '\n') {
        idx--;
    }

    return idx;
}

static size_t line_end(size_t idx) {
    while (idx < app.editor.len && app.editor.buf[idx] != '\n') {
        idx++;
    }

    return idx;
}

static void index_to_rowcol(size_t idx, size_t *row, size_t *col) {
    if (!row || !col) {
        return;
    }

    if (idx > app.editor.len) {
        idx = app.editor.len;
    }

    *row = 0;
    *col = 0;

    for (size_t i = 0; i < idx; i++) {
        if (app.editor.buf[i] == '\n') {
            (*row)++;
            *col = 0;
        } else {
            *col = visual_advance(*col, app.editor.buf[i]);
        }
    }
}

static size_t line_visual_col_at_index(size_t start, size_t idx) {
    if (idx < start) {
        idx = start;
    }

    size_t end = line_end(start);
    if (idx > end) {
        idx = end;
    }

    size_t col = 0;
    for (size_t i = start; i < idx; i++) {
        col = visual_advance(col, app.editor.buf[i]);
    }

    return col;
}

static size_t line_index_from_visual_col(size_t start, size_t target_col) {
    size_t end = line_end(start);
    size_t idx = start;
    size_t col = 0;

    while (idx < end) {
        size_t next_col = visual_advance(col, app.editor.buf[idx]);
        if (next_col > target_col) {
            break;
        }

        col = next_col;
        idx++;
    }

    return idx;
}

static size_t row_to_index(size_t row) {
    size_t idx = 0;

    while (idx < app.editor.len && row) {
        if (app.editor.buf[idx] == '\n') {
            row--;
        }
        idx++;
    }

    return idx;
}

static void keep_cursor_visible(void) {
    size_t row = 0;
    size_t col = 0;
    index_to_rowcol(app.editor.cursor, &row, &col);

    if (row < app.editor.rowoff) {
        app.editor.rowoff = row;
    }

    if (row >= app.editor.rowoff + app.editor.edit_rows) {
        app.editor.rowoff = row - app.editor.edit_rows + 1;
    }

    if (col < app.editor.coloff) {
        app.editor.coloff = col;
    }

    if (app.editor.cols && col >= app.editor.coloff + app.editor.cols) {
        app.editor.coloff = col - app.editor.cols + 1;
    }
}

static bool vi_grow(size_t needed) {
    if (needed <= app.editor.cap) {
        return true;
    }

    size_t new_cap = app.editor.cap;
    while (new_cap < needed) {
        new_cap = new_cap < 4096 ? 4096 : new_cap * 2;
    }

    char *p = realloc(app.editor.buf, new_cap);
    if (!p) {
        return false;
    }

    app.editor.buf = p;
    app.editor.cap = new_cap;
    return true;
}

static bool insert_char(size_t at, char ch) {
    if (!vi_grow(app.editor.len + 2)) {
        return false;
    }

    if (at > app.editor.len) {
        at = app.editor.len;
    }

    memmove(app.editor.buf + at + 1, app.editor.buf + at, app.editor.len - at);
    app.editor.buf[at] = ch;
    app.editor.len++;
    app.editor.buf[app.editor.len] = '\0';
    app.editor.dirty = true;
    app.editor.redraw = true;
    return true;
}

static void wrap_insert_line_if_needed(void) {
    if (!app.editor.cols || app.editor.mode != VI_INSERT) {
        return;
    }

    size_t start = line_start(app.editor.cursor);
    size_t col = line_visual_col_at_index(start, app.editor.cursor);

    if (col < app.editor.cols) {
        return;
    }

    if (insert_char(app.editor.cursor, '\n')) {
        app.editor.cursor++;
    }
}

static void insert_and_advance(char ch) {
    if (insert_char(app.editor.cursor, ch)) {
        app.editor.cursor++;

        if (ch != '\n') {
            wrap_insert_line_if_needed();
        }
    }
}

static bool delete_char(size_t at) {
    if (at >= app.editor.len) {
        return false;
    }

    memmove(app.editor.buf + at, app.editor.buf + at + 1, app.editor.len - at - 1);
    app.editor.len--;
    app.editor.buf[app.editor.len] = '\0';
    app.editor.dirty = true;
    app.editor.redraw = true;
    return true;
}

static bool delete_range(size_t at, size_t count) {
    if (!count || at >= app.editor.len) {
        return false;
    }

    if (at + count > app.editor.len) {
        count = app.editor.len - at;
    }

    memmove(app.editor.buf + at, app.editor.buf + at + count, app.editor.len - at - count);
    app.editor.len -= count;
    app.editor.buf[app.editor.len] = '\0';
    app.editor.dirty = true;
    app.editor.redraw = true;
    return true;
}

static void delete_current_line(void) {
    size_t start = line_start(app.editor.cursor);
    size_t end = line_end(start);
    size_t count = end - start;

    if (end < app.editor.len && app.editor.buf[end] == '\n') {
        count++;
    }

    if (!delete_range(start, count)) {
        return;
    }

    if (!app.editor.len) {
        app.editor.cursor = 0;
        return;
    }

    if (start >= app.editor.len) {
        app.editor.cursor = line_start(app.editor.len);
        if (app.editor.cursor == app.editor.len && app.editor.cursor > 0 &&
            app.editor.buf[app.editor.cursor - 1] == '\n') {
            app.editor.cursor = line_start(app.editor.cursor - 1);
        }
    } else {
        app.editor.cursor = start;
    }
}

static void move_cursor(int key) {
    size_t start = line_start(app.editor.cursor);
    size_t end = line_end(start);
    size_t col = line_visual_col_at_index(start, app.editor.cursor);

    switch (key) {
    case VI_KEY_LEFT:
        if (app.editor.cursor > start) {
            app.editor.cursor--;
        }
        break;
    case VI_KEY_RIGHT:
        if (app.editor.cursor < end) {
            app.editor.cursor++;
        }
        break;
    case VI_KEY_HOME:
        app.editor.cursor = start;
        break;
    case VI_KEY_END:
        app.editor.cursor = end;
        break;
    case VI_KEY_UP: {
        if (!start) {
            break;
        }

        size_t prev_end = start - 1;
        size_t prev_start = line_start(prev_end);
        app.editor.cursor = line_index_from_visual_col(prev_start, col);
        break;
    }
    case VI_KEY_DOWN: {
        if (end >= app.editor.len) {
            break;
        }

        size_t next_start = end + 1;
        app.editor.cursor = line_index_from_visual_col(next_start, col);
        break;
    }
    default:
        return;
    }

    app.editor.redraw = true;
}

static bool save_file(void) {
    if (!app.editor.path || !app.editor.path[0]) {
        set_msg("No file name");
        return false;
    }

    int fd = open(app.editor.path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        set_errno_msg("write");
        return false;
    }

    size_t off = 0;
    while (off < app.editor.len) {
        ssize_t n = write(fd, app.editor.buf + off, app.editor.len - off);
        if (n <= 0) {
            close(fd);
            set_errno_msg("write");
            return false;
        }
        off += (size_t)n;
    }

    close(fd);
    app.editor.dirty = false;

    char msg[VI_MSG_MAX];
    snprintf(msg, sizeof(msg), "\"%s\" %u bytes written", app.editor.path, (unsigned)app.editor.len);
    set_msg(msg);
    app.editor.redraw = true;
    return true;
}

static bool load_file(void) {
    if (!app.editor.path || !app.editor.path[0]) {
        clear_msg();
        return true;
    }

    int fd = open(app.editor.path, O_RDONLY, 0);
    if (fd < 0) {
        if (errno == ENOENT) {
            clear_msg();
            return true;
        }

        set_errno_msg("open");
        return false;
    }

    for (;;) {
        if (!vi_grow(app.editor.len + 4096)) {
            close(fd);
            set_msg("app.out of memory");
            return false;
        }

        ssize_t n = read(fd, app.editor.buf + app.editor.len, app.editor.cap - app.editor.len - 1);
        if (!n) {
            break;
        }

        if (n < 0) {
            close(fd);
            set_errno_msg("read");
            return false;
        }

        app.editor.len += (size_t)n;
    }

    close(fd);
    app.editor.buf[app.editor.len] = '\0';

    clear_msg();
    return true;
}

static const char *mode_name(void) {
    if (app.editor.mode == VI_INSERT) {
        return "INSERT";
    }
    if (app.editor.mode == VI_COMMAND) {
        return "COMMAND";
    }
    return "NORMAL";
}

static void build_editor_row(size_t *idx, char *dst) {
    memset(dst, ' ', app.editor.cols);
    if (!idx || *idx >= app.editor.len) {
        if (app.editor.cols) {
            dst[0] = '~';
        }
        return;
    }

    size_t end = line_end(*idx);
    size_t visual_col = 0;
    size_t dst_col = 0;

    for (size_t i = *idx; i < end && dst_col < app.editor.cols; i++) {
        char c = app.editor.buf[i];
        size_t next_col = visual_advance(visual_col, c);

        if (c == '\t') {
            for (size_t t = visual_col; t < next_col && dst_col < app.editor.cols; t++) {
                if (t < app.editor.coloff) {
                    continue;
                }
                dst[dst_col++] = ' ';
            }
        } else {
            if (next_col > app.editor.coloff) {
                char outc = c;
                if ((unsigned char)outc < 0x20) {
                    outc = '?';
                }

                if (visual_col >= app.editor.coloff && dst_col < app.editor.cols) {
                    dst[dst_col++] = outc;
                }
            }
        }

        visual_col = next_col;

        if (visual_col > app.editor.coloff + app.editor.cols) {
            break;
        }
    }

    *idx = end < app.editor.len ? end + 1 : end;
}

static size_t build_status_row(size_t row, size_t col, char *dst) {
    memset(dst, ' ', app.editor.cols);

    char right[64];
    snprintf(right, sizeof(right), "%s %u:%u ", mode_name(), (unsigned)(row + 1), (unsigned)(col + 1));

    size_t right_len = strlen(right);
    if (right_len > app.editor.cols) {
        right_len = app.editor.cols;
    }

    char left[256] = { 0 };
    if (app.editor.mode == VI_COMMAND) {
        snprintf(left, sizeof(left), ":%s", app.editor.cmd);
    } else if (app.editor.msg[0]) {
        snprintf(left, sizeof(left), "%s", app.editor.msg);
    }

    size_t left_max = app.editor.cols > right_len ? app.editor.cols - right_len : 0;
    size_t left_len = strlen(left);

    if (left_len > left_max) {
        left_len = left_max;
    }

    if (left_len) {
        memcpy(dst, left, left_len);
    }

    if (right_len) {
        memcpy(dst + (app.editor.cols - right_len), right, right_len);
    }

    return left_max;
}

static bool redraw_screen(void) {
    update_screen_size();
    keep_cursor_visible();

    bool resized = false;
    if (!ensure_row_scratch(app.editor.cols) ||
        !ensure_frame_cache(app.editor.cols, app.editor.edit_rows + 1, &resized)) {
        return false;
    }

    app.out.len = 0;
    if (!out_add("\x1b[?25l")) {
        return false;
    }

    if (resized) {
        memset(app.frame_cache, 0xff, app.frame_cache_cols * app.frame_cache_rows);
    }

    if (!out_add("\x1b[H")) {
        return false;
    }

    size_t row = 0;
    size_t col = 0;
    index_to_rowcol(app.editor.cursor, &row, &col);

    size_t idx = row_to_index(app.editor.rowoff);
    for (size_t screen_row = 0; screen_row < app.editor.edit_rows; screen_row++) {
        build_editor_row(&idx, app.row_scratch);

        if (!draw_row_if_changed(screen_row, app.row_scratch)) {
            return false;
        }
    }

    size_t command_limit = build_status_row(row, col, app.row_scratch);
    if (!draw_row_if_changed(app.editor.edit_rows, app.row_scratch)) {
        return false;
    }

    size_t x = 1;
    size_t y = 1;

    if (app.editor.mode == VI_COMMAND) {
        y = app.editor.edit_rows + 1;
        x = app.editor.cmd_len + 2;

        if (x > command_limit) {
            x = command_limit;
        }

        if (x < 2) {
            x = 2;
        }
    } else {
        if (row >= app.editor.rowoff) {
            y = (row - app.editor.rowoff) + 1;
        }

        if (col < app.editor.coloff) {
            x = 1;
        } else {
            x = (col - app.editor.coloff) + 1;
        }
    }

    if (x < 1) {
        x = 1;
    }

    if (y < 1) {
        y = 1;
    }

    if (x > app.editor.cols) {
        x = app.editor.cols;
    }

    if (y > app.editor.edit_rows + 1) {
        y = app.editor.edit_rows + 1;
    }

    char seq[32];
    snprintf(seq, sizeof(seq), "\x1b[%u;%uH", (unsigned)y, (unsigned)x);
    if (!out_add(seq) || !out_add("\x1b[?25h")) {
        return false;
    }

    if (!out_flush()) {
        return false;
    }

    app.editor.redraw = false;
    app.editor.repaint = false;
    return true;
}

static void run_command(void) {
    app.editor.cmd[app.editor.cmd_len] = '\0';

    if (!strcmp(app.editor.cmd, "w")) {
        save_file();
    } else if (!strcmp(app.editor.cmd, "q")) {
        if (app.editor.dirty) {
            set_msg("No write since last change (use :q!)");
        } else {
            quit_vi();
        }
    } else if (!strcmp(app.editor.cmd, "q!")) {
        quit_vi();
    } else if (!strcmp(app.editor.cmd, "wq") || !strcmp(app.editor.cmd, "x")) {
        if (save_file()) {
            quit_vi();
        }
    } else if (!strncmp(app.editor.cmd, "w ", 2)) {
        const char *name = app.editor.cmd + 2;
        while (*name == ' ') {
            name++;
        }

        if (!name[0]) {
            set_msg("missing file name");
        } else {
            snprintf(app.editor.path_buf, sizeof(app.editor.path_buf), "%s", name);
            app.editor.path = app.editor.path_buf;
            save_file();
        }
    } else {
        set_msg("unknown command");
    }

    clear_cmd();
    enter_mode(VI_NORMAL);
}

static void handle_insert(int key) {
    if (key == '\x1b') {
        enter_mode(VI_NORMAL);
        return;
    }

    if (key == '\r' || key == '\n') {
        insert_and_advance('\n');
        return;
    }

    if (key == '\b' || key == 0x7f) {
        if (app.editor.cursor > line_start(app.editor.cursor) && delete_char(app.editor.cursor - 1)) {
            app.editor.cursor--;
        }
        return;
    }

    if (key == VI_KEY_DEL) {
        delete_char(app.editor.cursor);
        return;
    }

    if (is_nav_key(key)) {
        move_cursor(key);
        return;
    }

    if (key == '\t') {
        insert_and_advance('\t');
        return;
    }

    if (key >= 0x20 && key <= 0x7e) {
        insert_and_advance((char)key);
    }
}

static void handle_normal(int key) {
    if (app.editor.pending_op) {
        if (app.editor.pending_op == 'd' && key == 'd') {
            app.editor.pending_op = 0;
            delete_current_line();
            return;
        }

        app.editor.pending_op = 0;
    }

    switch (key) {
    case ':':
        clear_cmd();
        enter_mode(VI_COMMAND);
        return;
    case 'i':
        enter_mode(VI_INSERT);
        return;
    case 'a':
        if (app.editor.cursor < line_end(line_start(app.editor.cursor))) {
            app.editor.cursor++;
        }
        enter_mode(VI_INSERT);
        return;
    case 'o': {
        size_t end = line_end(line_start(app.editor.cursor));
        if (end < app.editor.len) {
            end++;
        }

        if (insert_char(end, '\n')) {
            app.editor.cursor = end + 1;
            enter_mode(VI_INSERT);
        }
        return;
    }
    case 'x':
        delete_char(app.editor.cursor);
        return;
    case 'd':
        app.editor.pending_op = 'd';
        return;
    case 'h':
        move_cursor(VI_KEY_LEFT);
        return;
    case 'j':
        move_cursor(VI_KEY_DOWN);
        return;
    case 'k':
        move_cursor(VI_KEY_UP);
        return;
    case 'l':
        move_cursor(VI_KEY_RIGHT);
        return;
    case '0':
        app.editor.cursor = line_start(app.editor.cursor);
        app.editor.redraw = true;
        return;
    case '$':
        app.editor.cursor = line_end(line_start(app.editor.cursor));
        app.editor.redraw = true;
        return;
    case 'G':
        app.editor.cursor = app.editor.len;
        app.editor.redraw = true;
        return;
    default:
        if (is_nav_key(key)) {
            move_cursor(key);
        }
        return;
    }
}

static void handle_command_mode(int key) {
    if (key == '\x1b') {
        clear_cmd();
        enter_mode(VI_NORMAL);
        return;
    }

    if (key == '\r' || key == '\n') {
        run_command();
        return;
    }

    if (key == '\b' || key == 0x7f) {
        if (app.editor.cmd_len) {
            app.editor.cmd_len--;
            app.editor.cmd[app.editor.cmd_len] = '\0';
            app.editor.redraw = true;
        }
        return;
    }

    if (isprint((unsigned char)key) && app.editor.cmd_len + 1 < sizeof(app.editor.cmd)) {
        app.editor.cmd[app.editor.cmd_len++] = (char)key;
        app.editor.cmd[app.editor.cmd_len] = '\0';
        app.editor.redraw = true;
    }
}

static void sigwinch_handler(int signum) {
    (void)signum;
    app.got_sigwinch = 1;
}

static void install_signal_handlers(void) {
    signal(SIGWINCH, sigwinch_handler);
    signal(SIGSEGV, fatal_signal_handler);
    signal(SIGILL, fatal_signal_handler);
    signal(SIGBUS, fatal_signal_handler);
    signal(SIGTRAP, fatal_signal_handler);
}

int main(int argc, char *argv[]) {
    memset(&app.editor, 0, sizeof(app.editor));

    app.editor.buf = malloc(VI_INIT_CAP);
    app.out.data = malloc(VI_OUT_INIT);

    if (!app.editor.buf || !app.out.data) {
        io_write_str("app.editor: app.out of memory\n");
        return 1;
    }

    app.editor.cap = VI_INIT_CAP;
    app.editor.buf[0] = '\0';
    app.out.cap = VI_OUT_INIT;

    app.editor.running = true;
    app.editor.mode = VI_NORMAL;
    app.editor.redraw = true;

    if (argc > 1) {
        app.editor.path = argv[1];
    }

    if (!load_file()) {
        free(app.editor.buf);
        free(app.out.data);
        free(app.row_scratch);
        free(app.frame_cache);
        return 1;
    }

    if (!raw_mode_on()) {
        io_write_str("app.editor: failed to enter raw mode\n");
        free(app.editor.buf);
        free(app.out.data);
        free(app.row_scratch);
        free(app.frame_cache);
        return 1;
    }

    install_signal_handlers();

    screen_enter();
    detect_screen_size();

    while (app.editor.running) {
        if (app.editor.redraw) {
            if (!redraw_screen()) {
                set_msg("draw failed");
                app.editor.redraw = true;
            }
        }

        int key = read_key();
        if (key == VI_KEY_RESIZE) {
            app.editor.redraw = true;
            continue;
        }

        switch (app.editor.mode) {
        case VI_INSERT:
            handle_insert(key);
            break;
        case VI_COMMAND:
            handle_command_mode(key);
            break;
        default:
            handle_normal(key);
            break;
        }
    }

    raw_mode_off();
    screen_leave();

    free(app.editor.buf);
    free(app.out.data);
    free(app.row_scratch);
    free(app.frame_cache);

    return 0;
}
