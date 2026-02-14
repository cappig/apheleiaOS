#include "tty.h"

#include <arch/arch.h>
#include <errno.h>
#include <log/log.h>
#include <sched/scheduler.h>
#include <sched/signal.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/tty_input.h>
#include <termios.h>

static ssize_t current_tty = TTY_NONE;
static pid_t tty_pgrp[TTY_SCREEN_COUNT] = {0};

static bool _is_controlling_screen(const sched_thread_t* thread, size_t screen) {
    if (!thread || !thread->user_thread)
        return false;

    return thread->tty_index == (int)screen;
}

static bool _is_background_group(const sched_thread_t* thread, size_t screen) {
    if (!_is_controlling_screen(thread, screen))
        return false;

    pid_t fg_pgrp = tty_pgrp[screen];
    if (fg_pgrp <= 0 || thread->pgid <= 0)
        return false;

    return thread->pgid != fg_pgrp;
}

static ssize_t _check_foreground_read(size_t screen) {
    sched_thread_t* current = sched_current();
    if (!_is_background_group(current, screen))
        return 0;

    if (sched_signal_send_pgrp(current->pgid, SIGTTIN) < 0)
        return -EIO;

    return -EINTR;
}

static ssize_t _check_foreground_write(size_t screen) {
    sched_thread_t* current = sched_current();
    if (!_is_background_group(current, screen))
        return 0;

    termios_t tos;
    if (!tty_input_get_termios(screen, &tos))
        return 0;

    if (!(tos.c_lflag & TOSTOP))
        return 0;

    if (sched_signal_send_pgrp(current->pgid, SIGTTOU) < 0)
        return -EIO;

    return -EINTR;
}

static bool _resolve_screen(const tty_handle_t* handle, size_t* screen_out) {
    if (!handle || !screen_out)
        return false;

    switch (handle->kind) {
    case TTY_HANDLE_CURRENT:
        if (current_tty == TTY_NONE)
            return false;

        *screen_out = (size_t)current_tty;
        return true;
    case TTY_HANDLE_CONSOLE:
        *screen_out = TTY_CONSOLE;
        return true;
    case TTY_HANDLE_NAMED:
        if (handle->index >= TTY_COUNT)
            return false;

        *screen_out = TTY_USER_TO_SCREEN(handle->index);
        return true;
    default:
        return false;
    }
}

void tty_init(void) {
    if (!TTY_SCREEN_COUNT)
        return;

    current_tty = TTY_CONSOLE;

    tty_input_init();
    tty_input_set_current((size_t)current_tty);

    if (!arch_console_set_active((size_t)current_tty))
        log_warn("tty: failed to activate console screen %zu", (size_t)current_tty);
}

bool tty_set_current(size_t index) {
    if (index >= TTY_SCREEN_COUNT)
        return false;

    current_tty = (ssize_t)index;

    tty_input_set_current(index);

    if (!arch_console_set_active(index))
        log_warn("tty: failed to activate console screen %zu", index);

    return true;
}

pid_t tty_get_pgrp(size_t index) {
    if (index >= TTY_SCREEN_COUNT)
        return 0;

    return tty_pgrp[index];
}

size_t tty_current_screen(void) {
    if (current_tty == TTY_NONE)
        return TTY_CONSOLE;

    return (size_t)current_tty;
}

static ssize_t _read_screen(size_t index, void* buf, size_t len) {
    if (index >= TTY_SCREEN_COUNT)
        return -EINVAL;

    return tty_input_read(index, buf, len);
}

static ssize_t _write_screen(size_t index, const void* buf, size_t len) {
    if (index >= TTY_SCREEN_COUNT)
        return -EINVAL;

    return arch_console_write_screen(index, buf, len);
}

static ssize_t _write_screen_processed(size_t index, const void* buf, size_t len) {
    if (index >= TTY_SCREEN_COUNT || !buf || !len)
        return 0;

    termios_t tos;
    if (!tty_input_get_termios(index, &tos))
        return tty_write_screen(index, buf, len);

    if (!(tos.c_oflag & OPOST))
        return tty_write_screen(index, buf, len);

    bool has_cr = memchr(buf, '\r', len) != NULL;
    bool has_nl = memchr(buf, '\n', len) != NULL;
    bool needs_ocrnl = (tos.c_oflag & OCRNL) && has_cr;
    bool needs_onlret = (tos.c_oflag & ONLRET) && has_nl;
    bool needs_onlcr = (tos.c_oflag & ONLCR) && has_nl;

    // Fast path: no output post-processing transforms are needed.
    if (!needs_ocrnl && !needs_onlret && !needs_onlcr)
        return tty_write_screen(index, buf, len);

    const u8* in = buf;
    char out[256];
    size_t out_len = 0;

    for (size_t i = 0; i < len; i++) {
        char ch = (char)in[i];

        if ((tos.c_oflag & OCRNL) && ch == '\r')
            ch = '\n';

        if ((tos.c_oflag & ONLRET) && ch == '\n')
            ch = '\r';

        if ((tos.c_oflag & ONLCR) && ch == '\n') {
            if (out_len + 2 >= sizeof(out)) {
                tty_write_screen(index, out, out_len);
                out_len = 0;
            }
            out[out_len++] = '\r';
            out[out_len++] = '\n';
            continue;
        }

        if (out_len + 1 >= sizeof(out)) {
            tty_write_screen(index, out, out_len);
            out_len = 0;
        }

        out[out_len++] = ch;
    }

    if (out_len)
        tty_write_screen(index, out, out_len);

    return (ssize_t)len;
}

ssize_t tty_write_screen_output(size_t index, const void* buf, size_t len) {
    if (!buf || !len)
        return 0;

    return _write_screen_processed(index, buf, len);
}

ssize_t tty_read_handle(const tty_handle_t* handle, void* buf, size_t len) {
    if (!handle)
        return -EINVAL;

    size_t screen = 0;
    if (!_resolve_screen(handle, &screen))
        return -ENXIO;

    ssize_t fg_ret = _check_foreground_read(screen);
    if (fg_ret < 0)
        return fg_ret;

    switch (handle->kind) {
    case TTY_HANDLE_CURRENT:
        if (current_tty == TTY_NONE)
            return -ENXIO;

        return tty_read_screen((size_t)current_tty, buf, len);
    case TTY_HANDLE_CONSOLE:
        return tty_input_read(TTY_CONSOLE, buf, len);
    case TTY_HANDLE_NAMED:
        if (handle->index >= TTY_COUNT)
            return -EINVAL;

        return tty_read_screen(TTY_USER_TO_SCREEN(handle->index), buf, len);
    default:
        return -EINVAL;
    }
}

ssize_t tty_write_handle(const tty_handle_t* handle, const void* buf, size_t len) {
    if (!handle)
        return -EINVAL;

    size_t screen = 0;
    if (!_resolve_screen(handle, &screen))
        return -ENXIO;

    ssize_t fg_ret = _check_foreground_write(screen);
    if (fg_ret < 0)
        return fg_ret;

    switch (handle->kind) {
    case TTY_HANDLE_CURRENT:
        if (current_tty == TTY_NONE)
            return -ENXIO;

        return _write_screen_processed((size_t)current_tty, buf, len);
    case TTY_HANDLE_CONSOLE:
        return _write_screen_processed(TTY_CONSOLE, buf, len);
    case TTY_HANDLE_NAMED:
        if (handle->index >= TTY_COUNT)
            return -EINVAL;

        return _write_screen_processed(TTY_USER_TO_SCREEN(handle->index), buf, len);
    default:
        return -EINVAL;
    }
}

ssize_t tty_ioctl_handle(const tty_handle_t* handle, u64 request, void* args) {
    size_t screen = 0;
    if (!_resolve_screen(handle, &screen))
        return -ENXIO;

    switch (request) {
    case TIOCGWINSZ:
        if (!args)
            return -EINVAL;

        return tty_input_get_winsize(screen, args) ? 0 : -EIO;
    case TIOCSWINSZ:
        if (!args)
            return -EINVAL;

        return tty_input_set_winsize(screen, args) ? 0 : -EIO;
    case TCGETS:
        if (!args)
            return -EINVAL;

        return tty_input_get_termios(screen, args) ? 0 : -EIO;
    case TCSETS:
        if (!args)
            return -EINVAL;

        return tty_input_set_termios(screen, args, TTY_TERMIOS_SET_NONE) ? 0 : -EIO;
    case TCSETSW:
        if (!args)
            return -EINVAL;

        return tty_input_set_termios(screen, args, TTY_TERMIOS_SET_NONE) ? 0 : -EIO;
    case TCSETSF:
        if (!args)
            return -EINVAL;

        return tty_input_set_termios(screen, args, TTY_TERMIOS_SET_FLUSH) ? 0 : -EIO;
    case TIOCSPGRP:
        if (!args)
            return -EINVAL;

        pid_t requested = *(pid_t*)args;
        if (requested <= 0)
            return -EINVAL;

        sched_thread_t* current = sched_current();
        if (!current || !current->user_thread)
            return -EPERM;

        if (!tty_is_controlling_screen(current, screen))
            return -ENOTTY;

        if (current->sid <= 0 || !sched_pgrp_in_session(requested, current->sid))
            return -EPERM;

        tty_pgrp[screen] = requested;
        return 0;
    case TIOCGPGRP:
        if (!args)
            return -EINVAL;

        *(pid_t*)args = tty_pgrp[screen];
        return 0;
    default:
        return -ENOTTY;
    }
}
