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

static vi_t vi;
static vi_out_t out;
static volatile sig_atomic_t vi_got_sigwinch = 0;
static char *frame_cache = NULL;
static size_t frame_cache_cols = 0;
static size_t frame_cache_rows = 0;
static char *row_scratch = NULL;
static size_t row_scratch_cap = 0;
static char key_push = 0;
static bool key_push_valid = false;

static void raw_mode_off(void);

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
    if (row_scratch_cap >= need) {
        return true;
    }

    char *p = realloc(row_scratch, need);
    if (!p) {
        return false;
    }

    row_scratch = p;
    row_scratch_cap = need;
    return true;
}

static bool ensure_frame_cache(size_t cols, size_t rows, bool *resized_out) {
    if (resized_out) {
        *resized_out = false;
    }

    if (frame_cache && frame_cache_cols == cols && frame_cache_rows == rows) {
        return true;
    }

    if (!cols || !rows) {
        free(frame_cache);

        frame_cache = NULL;
        frame_cache_cols = 0;
        frame_cache_rows = 0;

        if (resized_out) {
            *resized_out = true;
        }

        return true;
    }

    size_t total = cols * rows;
    char *p = realloc(frame_cache, total);
    if (!p) {
        return false;
    }

    frame_cache = p;
    frame_cache_cols = cols;
    frame_cache_rows = rows;

    memset(frame_cache, 0xff, total);

    if (resized_out) {
        *resized_out = true;
    }

    vi.repaint = true;
    return true;
}

static bool out_addn(const char *text, size_t n) {
    if (!text || !n) {
        return true;
    }

    if (out.len + n > out.cap) {
        size_t need = out.len + n;
        size_t cap = out.cap ? out.cap : VI_OUT_INIT;

        while (cap < need) {
            cap *= 2;
        }

        char *p = realloc(out.data, cap);
        if (!p) {
            return false;
        }

        out.data = p;
        out.cap = cap;
    }

    memcpy(out.data + out.len, text, n);
    out.len += n;
    return true;
}

static bool out_add(const char *text) {
    if (!text) {
        return true;
    }

    return out_addn(text, strlen(text));
}

static bool draw_row_if_changed(size_t row, const char *data) {
    if (!data || !frame_cache || row >= frame_cache_rows || vi.cols != frame_cache_cols) {
        return false;
    }

    char *cached = frame_cache + (row * frame_cache_cols);
    if (!vi.repaint && !memcmp(cached, data, vi.cols)) {
        return true;
    }

    char seq[32];
    snprintf(seq, sizeof(seq), "\x1b[%u;1H", (unsigned)(row + 1));

    size_t draw_cols = vi.cols;
    if (draw_cols > 1) {
        draw_cols--;
    }

    if (!out_add(seq) || !out_addn(data, draw_cols) || !out_add("\x1b[K")) {
        return false;
    }

    memcpy(cached, data, vi.cols);
    return true;
}

static bool out_flush(void) {
    return write_all_fd(STDOUT_FILENO, out.data, out.len);
}

static void set_msg(const char *text) {
    if (!text) {
        return;
    }

    snprintf(vi.msg, sizeof(vi.msg), "%s", text);
}

static void clear_msg(void) {
    vi.msg[0] = '\0';
}

static void set_errno_msg(const char *op) {
    char msg[VI_MSG_MAX];
    snprintf(msg, sizeof(msg), "%s failed: %s", op, strerror(errno));
    set_msg(msg);
}

static bool is_nav_key(int key) {
    return (
        key == VI_KEY_LEFT ||
        key == VI_KEY_RIGHT ||
        key == VI_KEY_UP ||
        key == VI_KEY_DOWN ||
        key == VI_KEY_HOME ||
        key == VI_KEY_END
    );
}

static void clear_cmd(void) {
    vi.cmd_len = 0;
    vi.cmd[0] = '\0';
}

static void quit_vi(void) {
    vi.running = false;
}

static void screen_enter(void) {
    const char seq[] =
        "\r\x1b[0m\x1b[2K"
        "\x1b[?1049h"
        "\x1b[?25l\x1b[H\x1b[2J\x1b[3J\x1b[H";

    (void)write_all_fd(STDOUT_FILENO, seq, sizeof(seq) - 1);
    vi.repaint = true;
}

static void screen_leave(void) {
    const char seq[] = "\x1b[?25h\x1b[0m\x1b[?1049l\r\x1b[K";
    (void)write_all_fd(STDOUT_FILENO, seq, sizeof(seq) - 1);
}

static void enter_mode(vi_mode_t mode) {
    vi.pending_op = 0;
    vi.mode = mode;
    vi.redraw = true;
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

    vi.cols = size.cols;
    vi.edit_rows = size.rows > 1 ? size.rows - 1 : 1;
}

static bool raw_mode_on(void) {
    termios_t tos = {0};
    if (ioctl(STDIN_FILENO, TCGETS, &tos)) {
        return false;
    }

    vi.saved_tty = tos;
    vi.saved_tty_valid = true;

    tos.c_iflag &= (tcflag_t) ~(ICRNL | IXON | BRKINT | INPCK | ISTRIP);
    tos.c_oflag &= (tcflag_t) ~(OPOST);
    tos.c_cflag |= (tcflag_t)CS8;
    tos.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    tos.c_cc[VMIN] = 1;
    tos.c_cc[VTIME] = 0;

    return !ioctl(STDIN_FILENO, TCSETS, &tos);
}

static void raw_mode_off(void) {
    if (!vi.saved_tty_valid) {
        return;
    }

    ioctl(STDIN_FILENO, TCSETS, &vi.saved_tty);
}

static void key_push_back(char ch) {
    key_push = ch;
    key_push_valid = true;
}

static bool read_key_byte(char *out_ch, int timeout_ms) {
    if (!out_ch) {
        return false;
    }

    if (key_push_valid) {
        key_push_valid = false;
        *out_ch = key_push;
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
                if (vi_got_sigwinch) {
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
            if (vi_got_sigwinch) {
                return false;
            }

            continue;
        }

        return false;
    }
}

static bool read_term_byte(
    int fd,
    char *out_ch,
    int timeout_ms,
    void *ctx
) {
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
    char seq[16] = {0};
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
    term_size_t size = {0};

    bool probed = term_probe_size(
        STDIN_FILENO,
        STDOUT_FILENO,
        &size,
        read_term_byte,
        push_term_byte,
        NULL
    );
    if (!probed) {
        return false;
    }

    vi.cols = size.cols;
    vi.edit_rows = size.rows - 1;
    vi.repaint = true;
    vi.redraw = true;
    return true;
}

static int read_key(void) {
    if (vi_got_sigwinch) {
        vi_got_sigwinch = 0;
        return VI_KEY_RESIZE;
    }

    char ch = 0;
    if (!read_key_byte(&ch, -1)) {
        if (vi_got_sigwinch) {
            vi_got_sigwinch = 0;
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
    if (idx > vi.len) {
        idx = vi.len;
    }

    while (idx > 0 && vi.buf[idx - 1] != '\n') {
        idx--;
    }

    return idx;
}

static size_t line_end(size_t idx) {
    while (idx < vi.len && vi.buf[idx] != '\n') {
        idx++;
    }

    return idx;
}

static void index_to_rowcol(size_t idx, size_t *row, size_t *col) {
    if (!row || !col) {
        return;
    }

    if (idx > vi.len) {
        idx = vi.len;
    }

    *row = 0;
    *col = 0;

    for (size_t i = 0; i < idx; i++) {
        if (vi.buf[i] == '\n') {
            (*row)++;
            *col = 0;
        } else {
            *col = visual_advance(*col, vi.buf[i]);
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
        col = visual_advance(col, vi.buf[i]);
    }

    return col;
}

static size_t line_index_from_visual_col(size_t start, size_t target_col) {
    size_t end = line_end(start);
    size_t idx = start;
    size_t col = 0;

    while (idx < end) {
        size_t next_col = visual_advance(col, vi.buf[idx]);
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

    while (idx < vi.len && row) {
        if (vi.buf[idx] == '\n') {
            row--;
        }
        idx++;
    }

    return idx;
}

static void keep_cursor_visible(void) {
    size_t row = 0;
    size_t col = 0;
    index_to_rowcol(vi.cursor, &row, &col);

    if (row < vi.rowoff) {
        vi.rowoff = row;
    }

    if (row >= vi.rowoff + vi.edit_rows) {
        vi.rowoff = row - vi.edit_rows + 1;
    }

    if (col < vi.coloff) {
        vi.coloff = col;
    }

    if (vi.cols && col >= vi.coloff + vi.cols) {
        vi.coloff = col - vi.cols + 1;
    }
}

static bool vi_grow(size_t needed) {
    if (needed <= vi.cap) {
        return true;
    }

    size_t new_cap = vi.cap;
    while (new_cap < needed) {
        new_cap = new_cap < 4096 ? 4096 : new_cap * 2;
    }

    char *p = realloc(vi.buf, new_cap);
    if (!p) {
        return false;
    }

    vi.buf = p;
    vi.cap = new_cap;
    return true;
}

static bool insert_char(size_t at, char ch) {
    if (!vi_grow(vi.len + 2)) {
        return false;
    }

    if (at > vi.len) {
        at = vi.len;
    }

    memmove(vi.buf + at + 1, vi.buf + at, vi.len - at);
    vi.buf[at] = ch;
    vi.len++;
    vi.buf[vi.len] = '\0';
    vi.dirty = true;
    vi.redraw = true;
    return true;
}

static void wrap_insert_line_if_needed(void) {
    if (!vi.cols || vi.mode != VI_INSERT) {
        return;
    }

    size_t start = line_start(vi.cursor);
    size_t col = line_visual_col_at_index(start, vi.cursor);

    if (col < vi.cols) {
        return;
    }

    if (insert_char(vi.cursor, '\n')) {
        vi.cursor++;
    }
}

static void insert_and_advance(char ch) {
    if (insert_char(vi.cursor, ch)) {
        vi.cursor++;

        if (ch != '\n') {
            wrap_insert_line_if_needed();
        }
    }
}

static bool delete_char(size_t at) {
    if (at >= vi.len) {
        return false;
    }

    memmove(vi.buf + at, vi.buf + at + 1, vi.len - at - 1);
    vi.len--;
    vi.buf[vi.len] = '\0';
    vi.dirty = true;
    vi.redraw = true;
    return true;
}

static bool delete_range(size_t at, size_t count) {
    if (!count || at >= vi.len) {
        return false;
    }

    if (at + count > vi.len) {
        count = vi.len - at;
    }

    memmove(vi.buf + at, vi.buf + at + count, vi.len - at - count);
    vi.len -= count;
    vi.buf[vi.len] = '\0';
    vi.dirty = true;
    vi.redraw = true;
    return true;
}

static void delete_current_line(void) {
    size_t start = line_start(vi.cursor);
    size_t end = line_end(start);
    size_t count = end - start;

    if (end < vi.len && vi.buf[end] == '\n') {
        count++;
    }

    if (!delete_range(start, count)) {
        return;
    }

    if (!vi.len) {
        vi.cursor = 0;
        return;
    }

    if (start >= vi.len) {
        vi.cursor = line_start(vi.len);
        if (vi.cursor == vi.len && vi.cursor > 0 && vi.buf[vi.cursor - 1] == '\n') {
            vi.cursor = line_start(vi.cursor - 1);
        }
    } else {
        vi.cursor = start;
    }
}

static void move_cursor(int key) {
    size_t start = line_start(vi.cursor);
    size_t end = line_end(start);
    size_t col = line_visual_col_at_index(start, vi.cursor);

    switch (key) {
    case VI_KEY_LEFT:
        if (vi.cursor > start) {
            vi.cursor--;
        }
        break;
    case VI_KEY_RIGHT:
        if (vi.cursor < end) {
            vi.cursor++;
        }
        break;
    case VI_KEY_HOME:
        vi.cursor = start;
        break;
    case VI_KEY_END:
        vi.cursor = end;
        break;
    case VI_KEY_UP: {
        if (!start) {
            break;
        }

        size_t prev_end = start - 1;
        size_t prev_start = line_start(prev_end);
        vi.cursor = line_index_from_visual_col(prev_start, col);
        break;
    }
    case VI_KEY_DOWN: {
        if (end >= vi.len) {
            break;
        }

        size_t next_start = end + 1;
        vi.cursor = line_index_from_visual_col(next_start, col);
        break;
    }
    default:
        return;
    }

    vi.redraw = true;
}

static bool save_file(void) {
    if (!vi.path || !vi.path[0]) {
        set_msg("No file name");
        return false;
    }

    int fd = open(vi.path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        set_errno_msg("write");
        return false;
    }

    size_t off = 0;
    while (off < vi.len) {
        ssize_t n = write(fd, vi.buf + off, vi.len - off);
        if (n <= 0) {
            close(fd);
            set_errno_msg("write");
            return false;
        }
        off += (size_t)n;
    }

    close(fd);
    vi.dirty = false;

    char msg[VI_MSG_MAX];
    snprintf(
        msg, sizeof(msg), "\"%s\" %u bytes written", vi.path, (unsigned)vi.len
    );
    set_msg(msg);
    vi.redraw = true;
    return true;
}

static bool load_file(void) {
    if (!vi.path || !vi.path[0]) {
        clear_msg();
        return true;
    }

    int fd = open(vi.path, O_RDONLY, 0);
    if (fd < 0) {
        if (errno == ENOENT) {
            clear_msg();
            return true;
        }

        set_errno_msg("open");
        return false;
    }

    for (;;) {
        if (!vi_grow(vi.len + 4096)) {
            close(fd);
            set_msg("out of memory");
            return false;
        }

        ssize_t n = read(fd, vi.buf + vi.len, vi.cap - vi.len - 1);
        if (!n) {
            break;
        }

        if (n < 0) {
            close(fd);
            set_errno_msg("read");
            return false;
        }

        vi.len += (size_t)n;
    }

    close(fd);
    vi.buf[vi.len] = '\0';

    clear_msg();
    return true;
}

static const char *mode_name(void) {
    if (vi.mode == VI_INSERT) {
        return "INSERT";
    }
    if (vi.mode == VI_COMMAND) {
        return "COMMAND";
    }
    return "NORMAL";
}

static void build_editor_row(size_t *idx, char *dst) {
    memset(dst, ' ', vi.cols);
    if (!idx || *idx >= vi.len) {
        if (vi.cols) {
            dst[0] = '~';
        }
        return;
    }

    size_t end = line_end(*idx);
    size_t visual_col = 0;
    size_t dst_col = 0;

    for (size_t i = *idx; i < end && dst_col < vi.cols; i++) {
        char c = vi.buf[i];
        size_t next_col = visual_advance(visual_col, c);

        if (c == '\t') {
            for (size_t t = visual_col; t < next_col && dst_col < vi.cols; t++) {
                if (t < vi.coloff) {
                    continue;
                }
                dst[dst_col++] = ' ';
            }
        } else {
            if (next_col > vi.coloff) {
                char outc = c;
                if ((unsigned char)outc < 0x20) {
                    outc = '?';
                }

                if (visual_col >= vi.coloff && dst_col < vi.cols) {
                    dst[dst_col++] = outc;
                }
            }
        }

        visual_col = next_col;

        if (visual_col > vi.coloff + vi.cols) {
            break;
        }
    }

    *idx = end < vi.len ? end + 1 : end;
}

static size_t build_status_row(size_t row, size_t col, char *dst) {
    memset(dst, ' ', vi.cols);

    char right[64];
    snprintf(
        right,
        sizeof(right),
        "%s %u:%u ",
        mode_name(),
        (unsigned)(row + 1),
        (unsigned)(col + 1)
    );

    size_t right_len = strlen(right);
    if (right_len > vi.cols) {
        right_len = vi.cols;
    }

    char left[256] = {0};
    if (vi.mode == VI_COMMAND) {
        snprintf(left, sizeof(left), ":%s", vi.cmd);
    } else if (vi.msg[0]) {
        snprintf(left, sizeof(left), "%s", vi.msg);
    }

    size_t left_max = vi.cols > right_len ? vi.cols - right_len : 0;
    size_t left_len = strlen(left);

    if (left_len > left_max) {
        left_len = left_max;
    }

    if (left_len) {
        memcpy(dst, left, left_len);
    }

    if (right_len) {
        memcpy(dst + (vi.cols - right_len), right, right_len);
    }

    return left_max;
}

static bool redraw_screen(void) {
    update_screen_size();
    keep_cursor_visible();

    bool resized = false;
    if (!ensure_row_scratch(vi.cols) || !ensure_frame_cache(vi.cols, vi.edit_rows + 1, &resized)) {
        return false;
    }

    out.len = 0;
    if (!out_add("\x1b[?25l")) {
        return false;
    }

    if (resized) {
        memset(frame_cache, 0xff, frame_cache_cols * frame_cache_rows);
    }

    if (!out_add("\x1b[H")) {
        return false;
    }

    size_t row = 0;
    size_t col = 0;
    index_to_rowcol(vi.cursor, &row, &col);

    size_t idx = row_to_index(vi.rowoff);
    for (size_t screen_row = 0; screen_row < vi.edit_rows; screen_row++) {
        build_editor_row(&idx, row_scratch);

        if (!draw_row_if_changed(screen_row, row_scratch)) {
            return false;
        }
    }

    size_t command_limit = build_status_row(row, col, row_scratch);
    if (!draw_row_if_changed(vi.edit_rows, row_scratch)) {
        return false;
    }

    size_t x = 1;
    size_t y = 1;

    if (vi.mode == VI_COMMAND) {
        y = vi.edit_rows + 1;
        x = vi.cmd_len + 2;

        if (x > command_limit) {
            x = command_limit;
        }

        if (x < 2) {
            x = 2;
        }
    } else {
        if (row >= vi.rowoff) {
            y = (row - vi.rowoff) + 1;
        }

        if (col < vi.coloff) {
            x = 1;
        } else {
            x = (col - vi.coloff) + 1;
        }
    }

    if (x < 1) {
        x = 1;
    }

    if (y < 1) {
        y = 1;
    }

    if (x > vi.cols) {
        x = vi.cols;
    }

    if (y > vi.edit_rows + 1) {
        y = vi.edit_rows + 1;
    }

    char seq[32];
    snprintf(seq, sizeof(seq), "\x1b[%u;%uH", (unsigned)y, (unsigned)x);
    if (!out_add(seq) || !out_add("\x1b[?25h")) {
        return false;
    }

    if (!out_flush()) {
        return false;
    }

    vi.redraw = false;
    vi.repaint = false;
    return true;
}

static void run_command(void) {
    vi.cmd[vi.cmd_len] = '\0';

    if (!strcmp(vi.cmd, "w")) {
        save_file();
    } else if (!strcmp(vi.cmd, "q")) {
        if (vi.dirty) {
            set_msg("No write since last change (use :q!)");
        } else {
            quit_vi();
        }
    } else if (!strcmp(vi.cmd, "q!")) {
        quit_vi();
    } else if (!strcmp(vi.cmd, "wq") || !strcmp(vi.cmd, "x")) {
        if (save_file()) {
            quit_vi();
        }
    } else if (!strncmp(vi.cmd, "w ", 2)) {
        const char *name = vi.cmd + 2;
        while (*name == ' ') {
            name++;
        }

        if (!name[0]) {
            set_msg("missing file name");
        } else {
            snprintf(vi.path_buf, sizeof(vi.path_buf), "%s", name);
            vi.path = vi.path_buf;
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
        if (vi.cursor > line_start(vi.cursor) && delete_char(vi.cursor - 1)) {
            vi.cursor--;
        }
        return;
    }

    if (key == VI_KEY_DEL) {
        delete_char(vi.cursor);
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
    if (vi.pending_op) {
        if (vi.pending_op == 'd' && key == 'd') {
            vi.pending_op = 0;
            delete_current_line();
            return;
        }

        vi.pending_op = 0;
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
        if (vi.cursor < line_end(line_start(vi.cursor))) {
            vi.cursor++;
        }
        enter_mode(VI_INSERT);
        return;
    case 'o': {
        size_t end = line_end(line_start(vi.cursor));
        if (end < vi.len) {
            end++;
        }

        if (insert_char(end, '\n')) {
            vi.cursor = end + 1;
            enter_mode(VI_INSERT);
        }
        return;
    }
    case 'x':
        delete_char(vi.cursor);
        return;
    case 'd':
        vi.pending_op = 'd';
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
        vi.cursor = line_start(vi.cursor);
        vi.redraw = true;
        return;
    case '$':
        vi.cursor = line_end(line_start(vi.cursor));
        vi.redraw = true;
        return;
    case 'G':
        vi.cursor = vi.len;
        vi.redraw = true;
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
        if (vi.cmd_len) {
            vi.cmd_len--;
            vi.cmd[vi.cmd_len] = '\0';
            vi.redraw = true;
        }
        return;
    }

    if (isprint((unsigned char)key) && vi.cmd_len + 1 < sizeof(vi.cmd)) {
        vi.cmd[vi.cmd_len++] = (char)key;
        vi.cmd[vi.cmd_len] = '\0';
        vi.redraw = true;
    }
}

static void sigwinch_handler(int signum) {
    (void)signum;
    vi_got_sigwinch = 1;
}

static void install_signal_handlers(void) {
    signal(SIGWINCH, sigwinch_handler);
    signal(SIGSEGV, fatal_signal_handler);
    signal(SIGILL, fatal_signal_handler);
    signal(SIGBUS, fatal_signal_handler);
    signal(SIGTRAP, fatal_signal_handler);
}

int main(int argc, char *argv[]) {
    memset(&vi, 0, sizeof(vi));

    vi.buf = malloc(VI_INIT_CAP);
    out.data = malloc(VI_OUT_INIT);
    if (!vi.buf || !out.data) {
        io_write_str("vi: out of memory\n");
        return 1;
    }
    vi.cap = VI_INIT_CAP;
    vi.buf[0] = '\0';
    out.cap = VI_OUT_INIT;

    vi.running = true;
    vi.mode = VI_NORMAL;
    vi.redraw = true;

    if (argc > 1) {
        vi.path = argv[1];
    }

    if (!load_file()) {
        free(vi.buf);
        free(out.data);
        free(row_scratch);
        free(frame_cache);
        return 1;
    }

    if (!raw_mode_on()) {
        io_write_str("vi: failed to enter raw mode\n");
        free(vi.buf);
        free(out.data);
        free(row_scratch);
        free(frame_cache);
        return 1;
    }

    install_signal_handlers();

    screen_enter();
    detect_screen_size();

    while (vi.running) {
        if (vi.redraw) {
            if (!redraw_screen()) {
                set_msg("draw failed");
                vi.redraw = true;
            }
        }

        int key = read_key();
        if (key == VI_KEY_RESIZE) {
            vi.redraw = true;
            continue;
        }

        switch (vi.mode) {
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

    free(vi.buf);
    free(out.data);
    free(row_scratch);
    free(frame_cache);

    return 0;
}
