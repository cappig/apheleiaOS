#include "tty_input.h"

#include <arch/arch.h>
#include <ctype.h>
#include <data/ring.h>
#include <input/kbd.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <string.h>
#include <sys/tty.h>

static ring_buffer_t* tty_buffers[TTY_SCREEN_COUNT] = {0};
static sched_wait_queue_t tty_wait[TTY_SCREEN_COUNT] = {0};
static bool tty_ready[TTY_SCREEN_COUNT] = {0};
static char tty_line_buf[TTY_SCREEN_COUNT][TTY_INPUT_BUFFER_SIZE] = {{0}};
static size_t tty_line_len[TTY_SCREEN_COUNT] = {0};
static termios_t tty_termios[TTY_SCREEN_COUNT];
static winsize_t tty_winsize[TTY_SCREEN_COUNT];
static bool tty_termios_ready[TTY_SCREEN_COUNT] = {0};
static bool tty_literal_next[TTY_SCREEN_COUNT] = {0};
static bool tty_cr_pending[TTY_SCREEN_COUNT] = {0};

static void _tty_signal_flush(size_t screen, ring_buffer_t* buffer, const termios_t* tos) {
    if (!tos || (tos->c_lflag & NOFLSH))
        return;

    if (buffer)
        ring_buffer_clear(buffer);

    tty_line_len[screen] = 0;
    tty_literal_next[screen] = false;
    tty_cr_pending[screen] = false;
}

static void _tty_init_screen_state(size_t screen) {
    if (screen >= TTY_SCREEN_COUNT || tty_termios_ready[screen])
        return;

    __termios_default_init(&tty_termios[screen]);

    size_t cols = 80;
    size_t rows = 25;
    if (arch_console_get_size(&cols, &rows)) {
        tty_winsize[screen].ws_col = (unsigned short)cols;
        tty_winsize[screen].ws_row = (unsigned short)rows;
    } else {
        tty_winsize[screen].ws_col = 80;
        tty_winsize[screen].ws_row = 25;
    }

    tty_winsize[screen].ws_xpixel = 0;
    tty_winsize[screen].ws_ypixel = 0;

    tty_line_len[screen] = 0;
    tty_literal_next[screen] = false;
    tty_cr_pending[screen] = false;
    tty_termios_ready[screen] = true;
}

static ring_buffer_t* _buffer(size_t screen) {
    if (screen >= TTY_SCREEN_COUNT)
        return NULL;

    if (!tty_buffers[screen])
        tty_buffers[screen] = ring_buffer_create(TTY_INPUT_BUFFER_SIZE);

    if (!tty_buffers[screen]) {
        log_warn("tty: failed to allocate input buffer for screen %zu", screen);
        return NULL;
    }

    if (!tty_ready[screen]) {
        sched_wait_queue_init(&tty_wait[screen]);
        tty_ready[screen] = true;
    }

    _tty_init_screen_state(screen);
    return tty_buffers[screen];
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
    arch_console_write_screen(screen, bs_seq, sizeof(bs_seq) - 1);
}

static void _tty_echo_control(size_t screen, char ch) {
    char caret = ctrl_to_caret(ch);
    if (!caret)
        return;

    char out[2] = {'^', caret};
    arch_console_write_screen(screen, out, sizeof(out));
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

    arch_console_write_screen(screen, &ch, 1);
}

static void _tty_erase_chars(size_t screen, const termios_t* tos, size_t count) {
    if (!tos || count == 0)
        return;

    if (!(tos->c_lflag & ECHO))
        return;

    for (size_t i = 0; i < count; i++) {
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
    arch_console_write_screen(screen, &nl, 1);

    for (size_t i = 0; i < tty_line_len[screen]; i++)
        tty_echo_char(screen, tos, tty_line_buf[screen][i], false);
}

static void
_tty_flush_line(size_t screen, ring_buffer_t* buffer, bool add_newline, bool echo_newline) {
    if (!buffer || screen >= TTY_SCREEN_COUNT)
        return;

    size_t* line_len = &tty_line_len[screen];
    char* line = tty_line_buf[screen];

    if (*line_len)
        ring_buffer_push_array(buffer, (u8*)line, *line_len);

    if (add_newline)
        ring_buffer_push(buffer, (u8)'\n');

    *line_len = 0;

    if (echo_newline) {
        char nl = '\n';
        arch_console_write_screen(screen, &nl, 1);
    }

    sched_wake_one(&tty_wait[screen]);
}

static ssize_t
_read_raw(size_t screen, ring_buffer_t* buffer, void* buf, size_t len, const termios_t* tos) {
    if (!buffer || !buf || !tos || len == 0)
        return 0;

    size_t vmin = tos->c_cc[VMIN];
    size_t vtime = tos->c_cc[VTIME];

    if (vmin > len)
        vmin = len;

    if (vmin == 0 && vtime == 0) {
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
        if (timeout_ticks == 0)
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

            if (vmin == 0)
                return (ssize_t)total;

            if (total >= vmin)
                return (ssize_t)total;
        }

        if (timer_started && timeout_ticks) {
            u64 now = arch_timer_ticks();
            if (now - timer_start >= timeout_ticks)
                return (ssize_t)total;
        } else if (vmin == 0 && timeout_ticks) {
            timer_started = true;
            timer_start = arch_timer_ticks();
        }

        if (!sched_is_running())
            continue;

        sched_thread_t* current = sched_current();
        if (current && sched_signal_has_pending(current))
            return -1;

        if (timer_started && timeout_ticks) {
            sched_sleep(1);
            continue;
        }

        sched_block(&tty_wait[screen]);
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

void tty_input_push(char ch) {
    size_t screen = tty_current_screen();
    ring_buffer_t* buffer = tty_input_buffer(screen);

    if (!buffer || ch == '\0')
        return;

    char raw = ch;
    termios_t* tos = &tty_termios[screen];

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
            tty_cr_pending[screen] = raw_cr;
        } else if (raw_lf && tty_cr_pending[screen]) {
            tty_cr_pending[screen] = false;
            return;
        } else {
            tty_cr_pending[screen] = false;
        }
    }

    if (iexten && !tty_literal_next[screen] && ch == (char)tos->c_cc[VLNEXT]) {
        tty_literal_next[screen] = true;
        _tty_echo_char(screen, tos, ch, false);
        return;
    }

    bool literal = tty_literal_next[screen];
    if (literal)
        tty_literal_next[screen] = false;

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
                sched_signal_send_pid(pgrp, sig);

            if (canon) {
                if (tos->c_lflag & ECHOCTL)
                    tty_echo_control(screen, ch);
                if (tos->c_lflag & ECHO) {
                    char nl = '\n';
                    arch_console_write_screen(screen, &nl, 1);
                }
                tty_line_len[screen] = 0;
            }

            _tty_signal_flush(screen, buffer, tos);
            return;
        }
    }

    if (canon && !literal) {
        if (ch == (char)tos->c_cc[VERASE]) {
            size_t* line_len = &tty_line_len[screen];
            if (*line_len) {
                (*line_len)--;
                if (tos->c_lflag & ECHO) {
                    if (tos->c_lflag & ECHOE)
                        _tty_echo_backspace(screen);
                    else
                        _tty_echo_char(screen, tos, ch, false);
                }
            }
            return;
        }

        if (ch == (char)tos->c_cc[VKILL]) {
            size_t* line_len = &tty_line_len[screen];
            if (*line_len) {
                if (tos->c_lflag & ECHO) {
                    if (tos->c_lflag & ECHOE) {
                        while (*line_len) {
                            (*line_len)--;
                            _tty_echo_backspace(screen);
                        }
                    } else {
                        _tty_echo_char(screen, tos, ch, false);
                    }

                    if (tos->c_lflag & ECHOK) {
                        char nl = '\n';
                        arch_console_write_screen(screen, &nl, 1);
                    }
                }
            }

            *line_len = 0;
            return;
        }

        if (ch == (char)tos->c_cc[VWERASE]) {
            size_t* line_len = &tty_line_len[screen];
            if (*line_len) {
                size_t erase = 0;

                while (*line_len && isspace((unsigned char)tty_line_buf[screen][*line_len - 1])) {
                    (*line_len)--;
                    erase++;
                }

                while (*line_len && !isspace((unsigned char)tty_line_buf[screen][*line_len - 1])) {
                    (*line_len)--;
                    erase++;
                }

                _tty_erase_chars(screen, tos, erase);
            }
            return;
        }

        if (ch == (char)tos->c_cc[VREPRINT]) {
            if (tos->c_lflag & ECHOCTL)
                tty_echo_control(screen, ch);
            _tty_reprint_line(screen, tos);
            return;
        }

        if (ch == (char)tos->c_cc[VEOF]) {
            if (tty_line_len[screen] == 0) {
                ring_buffer_push(buffer, 0);
                sched_wake_one(&tty_wait[screen]);
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
        _tty_echo_char(screen, tos, ch, ch == '\n');
        sched_wake_one(&tty_wait[screen]);
        return;
    }

    if (iscntrl((unsigned char)ch) && ch != '\n' && ch != '\t') {
        _tty_echo_char(screen, tos, ch, false);
        return;
    }

    if (tty_line_len[screen] + 1 >= TTY_INPUT_BUFFER_SIZE)
        return;

    tty_line_buf[screen][tty_line_len[screen]++] = ch;
    _tty_echo_char(screen, tos, ch, false);
}

ssize_t tty_input_read(size_t screen, void* buf, size_t len) {
    if (!buf || !len || screen >= TTY_SCREEN_COUNT)
        return 0;

    ring_buffer_t* buffer = _buffer(screen);
    if (!buffer)
        return -1;

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
            if (popped == 1 && out[0] == 0)
                return 0;

            return (ssize_t)popped;
        }

        if (!sched_is_running()) {
            irq_restore(flags);
            continue;
        }

        sched_block_locked(&tty_wait[screen], flags);
    }
}

bool tty_input_get_termios(size_t screen, termios_t* out) {
    if (!out || screen >= TTY_SCREEN_COUNT)
        return false;

    _tty_init_screen_state(screen);
    memcpy(out, &tty_termios[screen], sizeof(*out));
    return true;
}

bool tty_input_set_termios(size_t screen, const termios_t* in, bool flush) {
    if (!in || screen >= TTY_SCREEN_COUNT)
        return false;

    _tty_init_screen_state(screen);
    memcpy(&tty_termios[screen], in, sizeof(*in));
    tty_literal_next[screen] = false;
    tty_cr_pending[screen] = false;

    if (flush)
        tty_input_flush(screen);

    return true;
}

bool tty_input_get_winsize(size_t screen, winsize_t* out) {
    if (!out || screen >= TTY_SCREEN_COUNT)
        return false;

    _tty_init_screen_state(screen);
    memcpy(out, &tty_winsize[screen], sizeof(*out));
    return true;
}

bool tty_input_set_winsize(size_t screen, const winsize_t* in) {
    if (!in || screen >= TTY_SCREEN_COUNT)
        return false;

    _tty_init_screen_state(screen);
    memcpy(&tty_winsize[screen], in, sizeof(*in));
    return true;
}

void tty_input_flush(size_t screen) {
    if (screen >= TTY_SCREEN_COUNT)
        return;

    ring_buffer_t* buffer = tty_input_buffer(screen);
    if (buffer)
        ring_buffer_clear(buffer);

    tty_line_len[screen] = 0;
    tty_literal_next[screen] = false;
    tty_cr_pending[screen] = false;
    tty_ready[screen] = true;
}
