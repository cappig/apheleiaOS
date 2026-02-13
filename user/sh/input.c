#include "input.h"

#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define SH_HISTORY_MAX 64

static volatile sig_atomic_t* sh_sigint = NULL;
static char sh_history[SH_HISTORY_MAX][SH_INPUT_LINE_MAX];
static size_t sh_history_count = 0;
static termios_t sh_tty_saved = {0};
static bool sh_tty_saved_valid = false;

typedef struct {
    size_t rows;
    size_t cursor_row;
} sh_render_state_t;

typedef struct {
    size_t cols;
    size_t prompt_cells;
} sh_layout_ctx_t;

static ssize_t write_str(const char* str) {
    if (!str)
        return 0;

    return write(STDOUT_FILENO, str, strlen(str));
}

static void ansi_clear_to_eos(void) {
    write_str("\x1b[J");
}

static void ansi_move_right(size_t count) {
    if (!count)
        return;

    char seq[32];
    snprintf(seq, sizeof(seq), "\x1b[%zuC", count);
    write_str(seq);
}

static void ansi_move_left(size_t count) {
    if (!count)
        return;

    char seq[32];
    snprintf(seq, sizeof(seq), "\x1b[%zuD", count);
    write_str(seq);
}

static void ansi_move_up(size_t count) {
    if (!count)
        return;

    char seq[32];
    snprintf(seq, sizeof(seq), "\x1b[%zuA", count);
    write_str(seq);
}

static size_t term_cols(void) {
    winsize_t ws = {0};
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) && ws.ws_col > 0)
        return (size_t)ws.ws_col;

    return 80;
}

static size_t normalize_cols(size_t cols) {
    return cols ? cols : 80;
}

static size_t display_cells(const char* buf, size_t len) {
    if (!buf)
        return 0;

    size_t cells = 0;
    for (size_t i = 0; i < len; i++) {
        if (((unsigned char)buf[i] & 0xc0) != 0x80)
            cells++;
    }

    return cells;
}

void input_set_sigint_flag(volatile sig_atomic_t* flag) {
    sh_sigint = flag;
}

static bool tty_read(termios_t* out) {
    if (!out)
        return false;

    return !ioctl(STDIN_FILENO, TCGETS, out);
}

static bool tty_write(const termios_t* in) {
    if (!in)
        return false;

    return !ioctl(STDIN_FILENO, TCSETS, (void*)in);
}

static bool tty_init_saved(void) {
    if (sh_tty_saved_valid)
        return true;

    if (!tty_read(&sh_tty_saved))
        return false;

    sh_tty_saved_valid = true;
    return true;
}

static bool tty_set_line_mode(bool edit_mode) {
    if (!tty_init_saved())
        return false;

    if (!edit_mode)
        return tty_write(&sh_tty_saved);

    termios_t tos = sh_tty_saved;
    tos.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
    tos.c_cc[VMIN] = 1;
    tos.c_cc[VTIME] = 0;
    return tty_write(&tos);
}

static bool got_sigint(void) {
    return sh_sigint && *sh_sigint;
}

static void clear_sigint(void) {
    if (sh_sigint)
        *sh_sigint = 0;
}

static size_t prev_char(const char* buf, size_t len) {
    if (!buf || !len)
        return 0;

    size_t i = len;
    do {
        i--;
    } while (i > 0 && (((unsigned char)buf[i] & 0xc0) == 0x80));

    return i;
}

static size_t next_char(const char* buf, size_t len, size_t pos) {
    if (!buf || pos >= len)
        return len;

    size_t i = pos + 1;
    while (i < len && (((unsigned char)buf[i] & 0xc0) == 0x80))
        i++;

    return i;
}

static void layout_line(
    const sh_layout_ctx_t* ctx,
    const char* buf,
    size_t len,
    size_t cursor,
    size_t* rows_out,
    size_t* cursor_row_out
) {
    if (!ctx || !buf || !rows_out || !cursor_row_out || cursor > len) {
        if (rows_out)
            *rows_out = 1;
        if (cursor_row_out)
            *cursor_row_out = 0;
        return;
    }

    size_t cols = normalize_cols(ctx->cols);
    size_t line_cells = display_cells(buf, len);
    size_t cursor_cells = display_cells(buf, cursor);
    size_t total_cells = ctx->prompt_cells + line_cells;
    size_t total_cursor_cells = ctx->prompt_cells + cursor_cells;

    *rows_out = (total_cells / cols) + 1;
    *cursor_row_out = total_cursor_cells / cols;
}

static bool cursor_on_wrap_boundary(const sh_layout_ctx_t* ctx, const char* buf, size_t cursor) {
    if (!ctx || !buf || !cursor)
        return false;

    size_t cols = normalize_cols(ctx->cols);
    size_t cursor_cells = display_cells(buf, cursor);
    size_t total = ctx->prompt_cells + cursor_cells;
    return !((total % cols));
}

static size_t total_cells(const sh_layout_ctx_t* ctx, const char* buf, size_t cursor) {
    if (!ctx || !buf)
        return 0;

    size_t cursor_cells = display_cells(buf, cursor);
    return ctx->prompt_cells + cursor_cells;
}

static void ansi_move_down(size_t count) {
    if (!count)
        return;

    char seq[32];
    snprintf(seq, sizeof(seq), "\x1b[%zuB", count);
    write_str(seq);
}

static void move_left_from_total(size_t total, size_t cells, size_t cols) {
    while (cells > 0 && total > 0) {
        size_t col = total % cols;
        if (!col) {
            write_str("\r");
            ansi_move_up(1);
            if (cols > 1)
                ansi_move_right(cols - 1);
            total--;
            cells--;
            continue;
        }

        size_t step = cells < col ? cells : col;
        ansi_move_left(step);
        total -= step;
        cells -= step;
    }
}

static void move_right_from_total(size_t total, size_t cells, size_t cols) {
    while (cells > 0) {
        size_t col = total % cols;
        if (col + 1 >= cols) {
            write_str("\r");
            ansi_move_down(1);
            total++;
            cells--;
            continue;
        }

        size_t room = cols - 1 - col;
        size_t step = cells < room ? cells : room;
        ansi_move_right(step);
        total += step;
        cells -= step;
    }
}

static void redraw_line(
    const char* prompt,
    const sh_layout_ctx_t* ctx,
    const char* buf,
    size_t len,
    size_t cursor,
    sh_render_state_t* state
) {
    if (!prompt || !ctx || !buf || cursor > len || !state)
        return;

    if (state->cursor_row)
        ansi_move_up(state->cursor_row);

    write_str("\r");
    ansi_clear_to_eos();

    write_str(prompt);
    if (len)
        write(STDOUT_FILENO, buf, len);

    if (cursor < len) {
        write_str("\r");
        write_str(prompt);
        if (cursor)
            write(STDOUT_FILENO, buf, cursor);
    }

    layout_line(ctx, buf, len, cursor, &state->rows, &state->cursor_row);
}

void history_add(const char* line) {
    if (!line || !line[0])
        return;

    if (sh_history_count > 0 && !strcmp(sh_history[sh_history_count - 1], line))
        return;

    if (sh_history_count < SH_HISTORY_MAX) {
        snprintf(sh_history[sh_history_count], sizeof(sh_history[sh_history_count]), "%s", line);
        sh_history_count++;
        return;
    }

    for (size_t i = 1; i < SH_HISTORY_MAX; i++)
        memcpy(sh_history[i - 1], sh_history[i], sizeof(sh_history[i - 1]));

    snprintf(sh_history[SH_HISTORY_MAX - 1], sizeof(sh_history[SH_HISTORY_MAX - 1]), "%s", line);
}

void history_print(void) {
    char line[320];

    for (size_t i = 0; i < sh_history_count; i++) {
        snprintf(line, sizeof(line), "%zu %s\n", i + 1, sh_history[i]);
        write_str(line);
    }
}

int read_line_interactive(const char* prompt, char* buf, size_t len, bool use_history) {
    if (!prompt || !buf || len < 2)
        return -1;

    if (!tty_set_line_mode(true))
        return -1;

    size_t pos = 0;
    size_t cursor = 0;
    sh_render_state_t render = {1, 0};
    sh_layout_ctx_t layout = {
        .cols = normalize_cols(term_cols()),
        .prompt_cells = display_cells(prompt, strlen(prompt)),
    };
    int history_cursor = -1;
    char scratch[SH_INPUT_LINE_MAX] = {0};
    buf[0] = '\0';

    write_str(prompt);

    for (;;) {
        if (got_sigint()) {
            clear_sigint();
            tty_set_line_mode(false);
            return -1;
        }

        char ch = 0;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0)
            continue;

        if (ch == '\r' || ch == '\n') {
            write_str("\n");
            buf[pos] = '\0';
            tty_set_line_mode(false);
            return 0;
        }

        if (ch == '\b' || (unsigned char)ch == 0x7f) {
            if (cursor > 0) {
                size_t old_cursor = cursor;
                size_t start = prev_char(buf, old_cursor);
                size_t removed_cells = display_cells(buf + start, old_cursor - start);
                if (!removed_cells)
                    removed_cells = 1;

                memmove(buf + start, buf + old_cursor, pos - old_cursor + 1);
                pos -= old_cursor - start;
                cursor = start;
                history_cursor = -1;
                size_t cols = layout.cols;

                if (cursor == pos && !cursor_on_wrap_boundary(&layout, buf, old_cursor)) {
                    for (size_t i = 0; i < removed_cells; i++)
                        write_str("\b \b");
                    layout_line(&layout, buf, pos, cursor, &render.rows, &render.cursor_row);
                } else {
                    size_t old_total = total_cells(&layout, buf, old_cursor);
                    move_left_from_total(old_total, removed_cells, cols);

                    size_t tail_bytes = pos - cursor;
                    if (tail_bytes)
                        write(STDOUT_FILENO, buf + cursor, tail_bytes);

                    write_str(" ");

                    size_t tail_cells = display_cells(buf + cursor, tail_bytes);
                    size_t start_total = total_cells(&layout, buf, cursor);
                    size_t end_total = start_total + tail_cells + 1;
                    move_left_from_total(end_total, tail_cells + 1, cols);
                    layout_line(&layout, buf, pos, cursor, &render.rows, &render.cursor_row);
                }
            }
            continue;
        }

        if (ch == '\x1b') {
            char seq1 = 0;
            char seq2 = 0;

            if (read(STDIN_FILENO, &seq1, 1) <= 0)
                continue;
            if (read(STDIN_FILENO, &seq2, 1) <= 0)
                continue;

            if (seq1 != '[')
                continue;

            if (seq2 == 'A' && use_history) {
                if (!sh_history_count)
                    continue;

                if (history_cursor < 0) {
                    snprintf(scratch, sizeof(scratch), "%s", buf);
                    history_cursor = (int)sh_history_count - 1;
                } else if (history_cursor > 0) {
                    history_cursor--;
                }

                snprintf(buf, len, "%s", sh_history[history_cursor]);
                pos = strlen(buf);
                cursor = pos;
                redraw_line(prompt, &layout, buf, pos, cursor, &render);
            } else if (seq2 == 'B' && use_history) {
                if (history_cursor < 0)
                    continue;

                if ((size_t)(history_cursor + 1) < sh_history_count) {
                    history_cursor++;
                    snprintf(buf, len, "%s", sh_history[history_cursor]);
                } else {
                    history_cursor = -1;
                    snprintf(buf, len, "%s", scratch);
                }

                pos = strlen(buf);
                cursor = pos;
                redraw_line(prompt, &layout, buf, pos, cursor, &render);
            } else if (seq2 == 'C') {
                if (cursor < pos) {
                    size_t cols = layout.cols;
                    size_t old_total = total_cells(&layout, buf, cursor);
                    size_t next = next_char(buf, pos, cursor);
                    cursor = next;
                    size_t new_total = total_cells(&layout, buf, cursor);
                    move_right_from_total(old_total, new_total - old_total, cols);
                    layout_line(&layout, buf, pos, cursor, &render.rows, &render.cursor_row);
                }
            } else if (seq2 == 'D') {
                if (cursor > 0) {
                    size_t cols = layout.cols;
                    size_t old_cursor = cursor;
                    size_t old_total = total_cells(&layout, buf, old_cursor);
                    size_t prev = prev_char(buf, cursor);
                    cursor = prev;
                    size_t move_cells = display_cells(buf + prev, old_cursor - prev);
                    if (!move_cells)
                        move_cells = 1;
                    move_left_from_total(old_total, move_cells, cols);
                    layout_line(&layout, buf, pos, cursor, &render.rows, &render.cursor_row);
                }
            }

            continue;
        }

        if ((unsigned char)ch < 0x20 && ch != '\t')
            continue;

        if (pos + 1 >= len)
            continue;

        history_cursor = -1;

        if (cursor == pos) {
            buf[pos++] = ch;
            cursor = pos;
            buf[pos] = '\0';
            write(STDOUT_FILENO, &ch, 1);
            layout_line(&layout, buf, pos, cursor, &render.rows, &render.cursor_row);
        } else {
            size_t old_cursor = cursor;
            memmove(buf + old_cursor + 1, buf + old_cursor, pos - old_cursor + 1);
            buf[old_cursor] = ch;
            pos++;
            cursor++;
            size_t cols = layout.cols;

            size_t rewrite_len = pos - old_cursor;
            write(STDOUT_FILENO, buf + old_cursor, rewrite_len);

            size_t tail_cells = display_cells(buf + cursor, pos - cursor);
            if (tail_cells) {
                size_t end_total = total_cells(&layout, buf, pos);
                move_left_from_total(end_total, tail_cells, cols);
            }

            layout_line(&layout, buf, pos, cursor, &render.rows, &render.cursor_row);
        }
    }
}
