#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CTRL(k) ((k) & 0x1f)

#define VI_INIT_CAP 4096
#define VI_CMD_MAX  64
#define VI_MSG_MAX  128
#define VI_OUT_MAX  (192 * 1024)

enum vi_key {
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

    char msg[VI_MSG_MAX];
    char cmd[VI_CMD_MAX];
    size_t cmd_len;

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

    return true;
}

static bool out_addn(const char *text, size_t n) {
    if (!text || !n) {
        return true;
    }

    if (out.len + n > out.cap) {
        return false;
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
    if (!memcmp(cached, data, vi.cols)) {
        return true;
    }

    char seq[32];
    snprintf(seq, sizeof(seq), "\x1b[%u;1H", (unsigned)(row + 1));

    if (!out_add(seq) || !out_addn(data, vi.cols)) {
        return false;
    }

    memcpy(cached, data, vi.cols);
    return true;
}

static bool out_flush(void) {
    size_t off = 0;
    while (off < out.len) {
        ssize_t n = write(STDOUT_FILENO, out.data + off, out.len - off);
        if (n <= 0) {
            return false;
        }
        off += (size_t)n;
    }
    return true;
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

static void enter_mode(vi_mode_t mode) {
    vi.mode = mode;
    vi.redraw = true;
}

static bool get_winsize(size_t *cols, size_t *rows) {
    if (!cols || !rows) {
        return false;
    }

    winsize_t ws = {0};

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws)) {
        memset(&ws, 0, sizeof(ws));


        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws)) {
            return false;
        }
    }

    if (!ws.ws_col || !ws.ws_row) {
        return false;
    }

    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return true;
}

static void update_screen_size(void) {
    size_t cols = 80;
    size_t rows = 25;

    get_winsize(&cols, &rows);

    if (cols < 20 || cols > 512) {
        cols = 80;
    }
    if (rows < 2 || rows > 256) {
        rows = 25;
    }

    vi.cols = cols;
    vi.edit_rows = rows > 1 ? rows - 1 : 1;
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
    tos.c_cc[VMIN] = 0;
    tos.c_cc[VTIME] = 1;

    return !ioctl(STDIN_FILENO, TCSETS, &tos);
}

static void raw_mode_off(void) {
    if (!vi.saved_tty_valid) {
        return;
    }

    ioctl(STDIN_FILENO, TCSETS, &vi.saved_tty);
}

static int read_key(void) {
    char ch = 0;
    for (;;) {
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n == 1) {
            break;
        }

        if (n < 0 && errno == EINTR) {
            if (vi_got_sigwinch) {
                vi_got_sigwinch = 0;
                return VI_KEY_RESIZE;
            }

            continue;
        }
    }

    if (ch != '\x1b') {
        return (unsigned char)ch;
    }

    char seq[3] = {0};
    if (read(STDIN_FILENO, &seq[0], 1) != 1) {
        return '\x1b';
    }
    if (read(STDIN_FILENO, &seq[1], 1) != 1) {
        return '\x1b';
    }

    if (seq[0] == '[') {
        switch (seq[1]) {
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
        case '3':
            if (read(STDIN_FILENO, &seq[2], 1) == 1 && seq[2] == '~') {
                return VI_KEY_DEL;
            }
            break;
        default:
            break;
        }
    }

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
            (*col)++;
        }
    }
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
    size_t col = vi.cursor - start;

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

static void move_cursor(int key) {
    size_t start = line_start(vi.cursor);
    size_t end = line_end(start);
    size_t col = vi.cursor - start;

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
        size_t prev_len = prev_end - prev_start;
        vi.cursor = prev_start + (col < prev_len ? col : prev_len);
        break;
    }
    case VI_KEY_DOWN: {
        if (end >= vi.len) {
            break;
        }

        size_t next_start = end + 1;
        size_t next_end = line_end(next_start);
        size_t next_len = next_end - next_start;
        vi.cursor = next_start + (col < next_len ? col : next_len);
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
    size_t start = *idx;

    if (vi.coloff < end - *idx) {
        start += vi.coloff;
    } else {
        start = end;
    }

    size_t n = end - start;
    if (n > vi.cols) {
        n = vi.cols;
    }

    for (size_t i = 0; i < n; i++) {
        char c = vi.buf[start + i];
        if (c == '\t') {
            c = ' ';
        }
        if ((unsigned char)c < 0x20) {
            c = '?';
        }
        dst[i] = c;
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

    if (resized && !out_add("\x1b[2J")) {
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
            vi.running = false;
        }
    } else if (!strcmp(vi.cmd, "q!")) {
        vi.running = false;
    } else if (!strcmp(vi.cmd, "wq") || !strcmp(vi.cmd, "x")) {
        if (save_file()) {
            vi.running = false;
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
    case CTRL('s'):
        save_file();
        return;
    case CTRL('q'):
        if (vi.dirty) {
            set_msg("unsaved changes (use :q!)");
        } else {
            vi.running = false;
        }
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

int main(int argc, char *argv[]) {
    memset(&vi, 0, sizeof(vi));

    vi.buf = malloc(VI_INIT_CAP);
    out.data = malloc(VI_OUT_MAX);
    if (!vi.buf || !out.data) {
        io_write_str("vi: out of memory\n");
        return 1;
    }
    vi.cap = VI_INIT_CAP;
    vi.buf[0] = '\0';
    out.cap = VI_OUT_MAX;

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

    signal(SIGWINCH, sigwinch_handler);

    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);

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
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);

    free(vi.buf);
    free(out.data);
    free(row_scratch);
    free(frame_cache);

    return 0;
}
