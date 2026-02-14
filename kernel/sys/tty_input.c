#include "tty_input.h"

#include <arch/arch.h>
#include <base/utf8.h>
#include <ctype.h>
#include <data/ring.h>
#include <errno.h>
#include <input/kbd.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <string.h>
#include <sys/console.h>
#include <sys/tty.h>

typedef struct {
    ring_buffer_t* buffer;
    sched_wait_queue_t wait;
    bool ready;
    char line_buf[TTY_INPUT_BUFFER_SIZE];
    size_t line_len;
    termios_t termios;
    winsize_t winsize;
    bool termios_ready;
    bool winsize_user_set;
    bool literal_next;
    bool cr_pending;
    size_t pending_newlines;
} tty_input_state_t;

static tty_input_state_t tty_state[TTY_SCREEN_COUNT] = {0};

static void _tty_signal_flush(size_t screen, ring_buffer_t* buffer, const termios_t* tos) {
    if (!tos || (tos->c_lflag & NOFLSH))
        return;

    if (buffer)
        ring_buffer_clear(buffer);

    tty_state[screen].line_len = 0;
    tty_state[screen].literal_next = false;
    tty_state[screen].cr_pending = false;
    tty_state[screen].pending_newlines = 0;
}

static void _tty_init_screen_state(size_t screen) {
    if (screen >= TTY_SCREEN_COUNT || tty_state[screen].termios_ready)
        return;

    __termios_default_init(&tty_state[screen].termios);

    size_t cols = 80;
    size_t rows = 25;
    if (console_get_size(&cols, &rows)) {
        tty_state[screen].winsize.ws_col = (unsigned short)cols;
        tty_state[screen].winsize.ws_row = (unsigned short)rows;
    } else {
        tty_state[screen].winsize.ws_col = 80;
        tty_state[screen].winsize.ws_row = 25;
    }

    tty_state[screen].winsize.ws_xpixel = 0;
    tty_state[screen].winsize.ws_ypixel = 0;

    tty_state[screen].line_len = 0;
    tty_state[screen].literal_next = false;
    tty_state[screen].cr_pending = false;
    tty_state[screen].pending_newlines = 0;
    tty_state[screen].winsize_user_set = false;
    tty_state[screen].termios_ready = true;
}

static ring_buffer_t* _buffer(size_t screen) {
    if (screen >= TTY_SCREEN_COUNT)
        return NULL;

    if (!tty_state[screen].buffer)
        tty_state[screen].buffer = ring_buffer_create(TTY_INPUT_BUFFER_SIZE);

    if (!tty_state[screen].buffer) {
        log_warn("tty: failed to allocate input buffer for screen %zu", screen);
        return NULL;
    }

    if (!tty_state[screen].ready) {
        sched_wait_queue_init(&tty_state[screen].wait);
        tty_state[screen].ready = true;
    }

    _tty_init_screen_state(screen);
    return tty_state[screen].buffer;
}

static bool _tty_apply_iflags(const termios_t* tos, char* ch) {
    if (!tos || !ch)
        return false;

    unsigned char c = (unsigned char)*ch;

    if (tos->c_iflag & ISTRIP)
        c &= 0x7f;

    if (tos->c_iflag & IUCLC)
        c = (unsigned char)tolower(c);

    if (c == '\r') {
        if (tos->c_iflag & IGNCR)
            return false;
        if (tos->c_iflag & ICRNL)
            c = '\n';
    } else if (c == '\n') {
        if (tos->c_iflag & INLCR)
            c = '\r';
    }

    *ch = (char)c;
    return true;
}

static void _tty_echo_backspace(size_t screen) {
    const char bs_seq[] = "\b \b";
    tty_write_screen_output(screen, bs_seq, sizeof(bs_seq) - 1);
}

static bool _tty_is_erase_char(const termios_t* tos, char ch) {
    if (!tos)
        return false;

    u8 c = (u8)ch;
    return c == (u8)tos->c_cc[VERASE] || c == (u8)'\b' || c == 0x7f;
}

static size_t _tty_line_prev_codepoint_len(const char* line, size_t line_len, u32* out_cp) {
    if (!line || !line_len)
        return 0;

    size_t start = line_len - 1;
    size_t continuation_bytes = 0;

    while (start > 0 && (((u8)line[start] & 0xc0) == 0x80) && continuation_bytes < 3) {
        start--;
        continuation_bytes++;
    }

    size_t bytes = line_len - start;
    size_t expected = utf8_sequence_len((u8)line[start]);

    if (expected && expected == bytes) {
        u32 cp = 0;
        if (utf8_decode((const u8*)&line[start], expected, &cp) == expected) {
            if (out_cp)
                *out_cp = cp;
            return expected;
        }
    }

    if (out_cp)
        *out_cp = (u8)line[line_len - 1];

    return 1;
}

static bool _tty_cp_isspace(u32 cp) {
    return cp < 0x80 && isspace((unsigned char)cp);
}

static void _tty_echo_control(size_t screen, char ch) {
    char caret = ctrl_to_caret(ch);
    if (!caret)
        return;

    char out[2] = {'^', caret};
    tty_write_screen_output(screen, out, sizeof(out));
}

static void _tty_echo_char(size_t screen, const termios_t* tos, char ch, bool force_newline) {
    if (!tos)
        return;

    bool echo = (tos->c_lflag & ECHO) != 0;
    bool echonl = (tos->c_lflag & ECHONL) != 0;
    bool echoctl = (tos->c_lflag & ECHOCTL) != 0;

    if (!echo) {
        if (!force_newline || !echonl)
            return;
    }

    if (iscntrl((unsigned char)ch) && ch != '\n' && ch != '\t') {
        if (echoctl)
            _tty_echo_control(screen, ch);
        return;
    }

    tty_write_screen_output(screen, &ch, 1);
}

static size_t _tty_echo_columns_for_cp(const termios_t* tos, u32 cp) {
    if (!tos)
        return 1;

    if (cp < 0x80 && iscntrl((unsigned char)cp) && cp != '\n' && cp != '\t') {
        if (tos->c_lflag & ECHOCTL)
            return 2;
        return 0;
    }

    return 1;
}

static bool
_tty_pop_ansi_sequence(size_t screen, const termios_t* tos, size_t* erase_bytes, size_t* erase_cols) {
    if (!tos || !erase_bytes || !erase_cols || screen >= TTY_SCREEN_COUNT)
        return false;

    size_t line_len = tty_state[screen].line_len;
    const char* line = tty_state[screen].line_buf;
    if (line_len < 2)
        return false;

    u8 final = (u8)line[line_len - 1];
    if (final < '@' || final > '~')
        return false;

    size_t i = line_len - 1;

    while (i > 0) {
        u8 c = (u8)line[i - 1];
        if (!(isdigit(c) || c == ';' || c == '?'))
            break;
        i--;
    }

    if (!i)
        return false;

    u8 intro = (u8)line[i - 1];
    if (intro != '[' && intro != 'O')
        return false;

    size_t start = i - 1;
    bool esc_stored = false;
    bool caret_prefix = false;

    if (start > 0 && (u8)line[start - 1] == 0x1b) {
        start--;
        esc_stored = true;
    } else if (start >= 2 && line[start - 2] == '^' && line[start - 1] == '[') {
        start -= 2;
        caret_prefix = true;
    }

    size_t bytes = line_len - start;
    size_t cols = 0;

    if (!esc_stored && !caret_prefix)
        cols += _tty_echo_columns_for_cp(tos, 0x1b);

    for (size_t j = start; j < line_len; j++)
        cols += _tty_echo_columns_for_cp(tos, (u8)line[j]);

    *erase_bytes = bytes;
    *erase_cols = cols;
    return true;
}

static void _tty_erase_columns(size_t screen, const termios_t* tos, size_t cols) {
    if (!tos || !cols)
        return;

    if (!(tos->c_lflag & ECHO))
        return;

    for (size_t i = 0; i < cols; i++) {
        if (tos->c_lflag & ECHOE)
            tty_echo_backspace(screen);
        else
            tty_echo_char(screen, tos, (char)tos->c_cc[VERASE], false);
    }
}

static void _tty_reprint_line(size_t screen, const termios_t* tos) {
    if (!tos || !(tos->c_lflag & ECHO))
        return;

    char nl = '\n';
    tty_write_screen_output(screen, &nl, 1);

    for (size_t i = 0; i < tty_state[screen].line_len; i++)
        tty_echo_char(screen, tos, tty_state[screen].line_buf[i], false);
}

static void
_tty_flush_line(size_t screen, ring_buffer_t* buffer, bool add_newline, bool echo_newline) {
    if (!buffer || screen >= TTY_SCREEN_COUNT)
        return;

    size_t* line_len = &tty_state[screen].line_len;
    char* line = tty_state[screen].line_buf;

    if (*line_len)
        ring_buffer_push_array(buffer, (u8*)line, *line_len);

    if (add_newline)
        ring_buffer_push(buffer, (u8)'\n');

    *line_len = 0;

    if (echo_newline)
        tty_state[screen].pending_newlines++;

    sched_wake_one(&tty_state[screen].wait);
}

static ssize_t
_read_raw(size_t screen, ring_buffer_t* buffer, void* buf, size_t len, const termios_t* tos) {
    if (!buffer || !buf || !tos || !len)
        return 0;

    size_t vmin = tos->c_cc[VMIN];
    size_t vtime = tos->c_cc[VTIME];

    if (vmin > len)
        vmin = len;

    if (!vmin && !vtime) {
        unsigned long flags = arch_irq_save();
        size_t popped = ring_buffer_pop_array(buffer, buf, len);
        arch_irq_restore(flags);
        return (ssize_t)popped;
    }

    size_t total = 0;
    bool timer_started = false;
    u64 timer_start = 0;
    u64 timeout_ticks = 0;

    if (vtime) {
        u32 hz = arch_timer_hz();
        timeout_ticks = ((u64)vtime * (u64)hz + 9) / 10;
        if (!timeout_ticks)
            timeout_ticks = 1;
    }

    for (;;) {
        unsigned long flags = arch_irq_save();
        size_t popped = ring_buffer_pop_array(buffer, (u8*)buf + total, len - total);

        arch_irq_restore(flags);

        if (popped) {
            total += popped;

            if (!timer_started && timeout_ticks) {
                timer_started = true;
                timer_start = arch_timer_ticks();
            }

            if (!vmin)
                return (ssize_t)total;

            if (total >= vmin)
                return (ssize_t)total;
        }

        if (timer_started && timeout_ticks) {
            u64 now = arch_timer_ticks();

            if (now - timer_start >= timeout_ticks)
                return (ssize_t)total;
        } else if (!vmin && timeout_ticks) {
            timer_started = true;
            timer_start = arch_timer_ticks();
        }

        if (!sched_is_running())
            continue;

        sched_thread_t* current = sched_current();
        if (current && sched_signal_has_pending(current))
            return -EINTR;

        if (timer_started && timeout_ticks) {
            sched_sleep(1);
            continue;
        }

        sched_block(&tty_state[screen].wait);
    }
}

void tty_input_init(void) {
    for (size_t i = 0; i < TTY_SCREEN_COUNT; i++)
        _buffer(i);
}

void tty_input_set_current(size_t screen) {
    if (screen >= TTY_SCREEN_COUNT)
        return;

    tty_input_buffer(screen);
}

static void _push_impl(char ch) {
    size_t screen = tty_current_screen();
    ring_buffer_t* buffer = tty_input_buffer(screen);

    if (!buffer || ch == '\0')
        return;

    char raw = ch;
    termios_t* tos = &tty_state[screen].termios;

    if (!_tty_apply_iflags(tos, &ch))
        return;

    bool canon = (tos->c_lflag & ICANON) != 0;
    bool iexten = (tos->c_lflag & IEXTEN) != 0;

    if (canon) {
        bool raw_cr = raw == '\r';
        bool raw_lf = raw == '\n';
        bool cooked_cr = ch == '\r';

        if (raw_cr || cooked_cr) {
            ch = '\n';
            tty_state[screen].cr_pending = raw_cr;
        } else if (raw_lf && tty_state[screen].cr_pending) {
            tty_state[screen].cr_pending = false;
            return;
        } else {
            tty_state[screen].cr_pending = false;
        }
    }

    if (iexten && !tty_state[screen].literal_next && ch == (char)tos->c_cc[VLNEXT]) {
        tty_state[screen].literal_next = true;
        tty_echo_char(screen, tos, ch, false);
        return;
    }

    bool literal = tty_state[screen].literal_next;
    if (literal)
        tty_state[screen].literal_next = false;

    if ((tos->c_lflag & ISIG) && !literal) {
        int sig = 0;

        if (ch == (char)tos->c_cc[VINTR])
            sig = SIGINT;
        else if (ch == (char)tos->c_cc[VQUIT])
            sig = SIGQUIT;
        else if (ch == (char)tos->c_cc[VSUSP])
            sig = SIGTSTP;

        if (sig) {
            pid_t pgrp = tty_get_pgrp(screen);
            if (!pgrp) {
                sched_thread_t* current = sched_current();

                if (current)
                    pgrp = current->pid;
            }

            if (pgrp)
                sched_signal_send_pgrp(pgrp, sig);

            if (canon) {
                if (tos->c_lflag & ECHOCTL)
                    tty_echo_control(screen, ch);

                if (tos->c_lflag & ECHO) {
                    char nl = '\n';
                    tty_write_screen_output(screen, &nl, 1);
                }

                tty_state[screen].line_len = 0;
            }

            _tty_signal_flush(screen, buffer, tos);
            return;
        }
    }

    if (canon && !literal) {
        if (_tty_is_erase_char(tos, ch)) {
            size_t* line_len = &tty_state[screen].line_len;

            if (*line_len) {
                size_t erased = 0;
                size_t erase_cols = 0;
                u32 cp = 0;

                if (!tty_pop_ansi_sequence(screen, tos, &erased, &erase_cols)) {
                    erased =
                        _tty_line_prev_codepoint_len(tty_state[screen].line_buf, *line_len, &cp);
                    erase_cols = tty_echo_columns_for_cp(tos, cp);
                }

                *line_len -= erased;

                if (tos->c_lflag & ECHO) {
                    if (tos->c_lflag & ECHOE)
                        _tty_erase_columns(screen, tos, erase_cols);
                    else
                        _tty_echo_char(screen, tos, ch, false);
                }
            }

            return;
        }

        if (ch == (char)tos->c_cc[VKILL]) {
            size_t* line_len = &tty_state[screen].line_len;

            if (*line_len) {
                size_t erase_cols = 0;

                while (*line_len) {
                    u32 cp = 0;
                    size_t erased =
                        tty_line_prev_codepoint_len(tty_state[screen].line_buf, *line_len, &cp);

                    if (!erased)
                        break;

                    *line_len -= erased;
                    erase_cols += _tty_echo_columns_for_cp(tos, cp);
                }

                if (tos->c_lflag & ECHO) {
                    if (tos->c_lflag & ECHOE) {
                        _tty_erase_columns(screen, tos, erase_cols);
                    } else {
                        _tty_echo_char(screen, tos, ch, false);
                    }

                    if (tos->c_lflag & ECHOK) {
                        char nl = '\n';
                        tty_write_screen_output(screen, &nl, 1);
                    }
                }
            }

            return;
        }

        if (ch == (char)tos->c_cc[VWERASE]) {
            size_t* line_len = &tty_state[screen].line_len;

            if (*line_len) {
                size_t erase_cols = 0;
                u32 cp = 0;

                while (*line_len) {
                    size_t erased =
                        tty_line_prev_codepoint_len(tty_state[screen].line_buf, *line_len, &cp);

                    if (!erased || !tty_cp_isspace(cp))
                        break;

                    *line_len -= erased;
                    erase_cols += _tty_echo_columns_for_cp(tos, cp);
                }

                while (*line_len) {
                    size_t erased =
                        tty_line_prev_codepoint_len(tty_state[screen].line_buf, *line_len, &cp);

                    if (!erased || tty_cp_isspace(cp))
                        break;

                    *line_len -= erased;
                    erase_cols += _tty_echo_columns_for_cp(tos, cp);
                }

                _tty_erase_columns(screen, tos, erase_cols);
            }

            return;
        }

        if (ch == (char)tos->c_cc[VREPRINT]) {
            if (tos->c_lflag & ECHOCTL)
                tty_echo_control(screen, ch);

            tty_reprint_line(screen, tos);

            return;
        }

        if (ch == (char)tos->c_cc[VEOF]) {
            if (!tty_state[screen].line_len) {
                ring_buffer_push(buffer, 0);
                sched_wake_one(&tty_state[screen].wait);
            } else {
                _tty_flush_line(screen, buffer, false, false);
            }

            return;
        }

        if (ch == (char)tos->c_cc[VEOL] || ch == '\n') {
            bool echo_newline = (tos->c_lflag & ECHO) != 0 || (tos->c_lflag & ECHONL) != 0;
            _tty_flush_line(screen, buffer, true, echo_newline);
            return;
        }
    }

    if (!canon) {
        ring_buffer_push(buffer, (u8)ch);
        tty_echo_char(screen, tos, ch, ch == '\n');
        sched_wake_one(&tty_state[screen].wait);
        return;
    }

    if (iscntrl((unsigned char)ch) && ch != '\n' && ch != '\t') {
        _tty_echo_char(screen, tos, ch, false);
        return;
    }

    if (tty_state[screen].line_len + 1 >= TTY_INPUT_BUFFER_SIZE)
        return;

    tty_state[screen].line_buf[tty_state[screen].line_len++] = ch;
    tty_echo_char(screen, tos, ch, false);
}

void tty_input_push(char ch) {
    _push_impl(ch);
}

ssize_t tty_input_read(size_t screen, void* buf, size_t len) {
    if (!buf || !len || screen >= TTY_SCREEN_COUNT)
        return 0;

    ring_buffer_t* buffer = _buffer(screen);
    if (!buffer)
        return -EIO;

    u8* out = buf;
    termios_t tos;
    bool have_termios = tty_input_get_termios(screen, &tos);

    if (have_termios && !(tos.c_lflag & ICANON))
        return _read_raw(screen, buffer, buf, len, &tos);

    for (;;) {
        unsigned long flags = arch_irq_save();
        size_t popped = ring_buffer_pop_array(buffer, out, len);
        arch_irq_restore(flags);

        if (popped) {
            if (popped == 1 && !out[0])
                return 0;

            if (tty_state[screen].pending_newlines) {
                for (size_t i = 0; i < popped; i++) {
                    if (out[i] != '\n')
                        continue;

                    if (!tty_state[screen].pending_newlines)
                        break;

                    char nl = '\n';
                    tty_write_screen_output(screen, &nl, 1);
                    tty_state[screen].pending_newlines--;
                }
            }

            return (ssize_t)popped;
        }

        if (!sched_is_running()) {
            irq_restore(flags);
            continue;
        }

        sched_thread_t* current = sched_current();
        if (current && sched_signal_has_pending(current))
            return -EINTR;

        sched_block(&tty_state[screen].wait);
    }
}

bool tty_input_get_termios(size_t screen, termios_t* out) {
    if (!out || screen >= TTY_SCREEN_COUNT)
        return false;

    _tty_init_screen_state(screen);
    memcpy(out, &tty_state[screen].termios, sizeof(*out));

    return true;
}

bool tty_input_set_termios(size_t screen, const termios_t* in, u32 flags) {
    if (!in || screen >= TTY_SCREEN_COUNT)
        return false;

    _tty_init_screen_state(screen);
    memcpy(&tty_state[screen].termios, in, sizeof(*in));
    tty_state[screen].literal_next = false;
    tty_state[screen].cr_pending = false;

    if (flags & TTY_TERMIOS_SET_FLUSH)
        tty_input_flush(screen);

    return true;
}

bool tty_input_get_winsize(size_t screen, winsize_t* out) {
    if (!out || screen >= TTY_SCREEN_COUNT)
        return false;

    tty_init_screen_state(screen);

    memcpy(out, &tty_state[screen].winsize, sizeof(*out));
    return true;
}

bool tty_input_set_winsize(size_t screen, const winsize_t* in) {
    if (!in || screen >= TTY_SCREEN_COUNT)
        return false;

    _tty_init_screen_state(screen);
    memcpy(&tty_state[screen].winsize, in, sizeof(*in));
    tty_state[screen].winsize_user_set = true;

    return true;
}

bool tty_input_has_data(size_t screen) {
    if (screen >= TTY_SCREEN_COUNT)
        return false;

    ring_buffer_t* buffer = tty_input_buffer(screen);
    if (!buffer)
        return false;

    unsigned long flags = arch_irq_save();
    bool ready = !ring_buffer_is_empty(buffer);
    arch_irq_restore(flags);

    return ready;
}

void tty_input_flush(size_t screen) {
    if (screen >= TTY_SCREEN_COUNT)
        return;

    ring_buffer_t* buffer = tty_input_buffer(screen);
    if (buffer)
        ring_buffer_clear(buffer);

    tty_state[screen].line_len = 0;
    tty_state[screen].literal_next = false;
    tty_state[screen].cr_pending = false;
    tty_state[screen].pending_newlines = 0;
    tty_state[screen].ready = true;
}
