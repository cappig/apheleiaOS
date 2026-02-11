#include "tty.h"

#include <arch/arch.h>
#include <log/log.h>
#include <sys/ioctl.h>
#include <sys/tty_input.h>
#include <termios.h>

static ssize_t current_tty = TTY_NONE;
static pid_t tty_pgrp[TTY_SCREEN_COUNT] = {0};

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
    if (TTY_SCREEN_COUNT == 0)
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
        return -1;

    return tty_input_read(index, buf, len);
}

static ssize_t _write_screen(size_t index, const void* buf, size_t len) {
    if (index >= TTY_SCREEN_COUNT)
        return -1;

    return arch_console_write_screen(index, buf, len);
}

static ssize_t _write_screen_processed(size_t index, const void* buf, size_t len) {
    if (index >= TTY_SCREEN_COUNT || !buf || len == 0)
        return 0;

    termios_t tos;
    if (!tty_input_get_termios(index, &tos))
        return tty_write_screen(index, buf, len);

    if (!(tos.c_oflag & OPOST))
        return tty_write_screen(index, buf, len);

    const u8* in = buf;
    char out[128];
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
    if (!buf || len == 0)
        return 0;

    if (len == 1 && *(const char*)buf == '\n') {
        const char crlf[] = {'\r', '\n'};
        return tty_write_screen(index, crlf, sizeof(crlf));
    }

    return tty_write_screen_processed(index, buf, len);
}

ssize_t tty_read_handle(const tty_handle_t* handle, void* buf, size_t len) {
    if (!handle)
        return -1;

    switch (handle->kind) {
    case TTY_HANDLE_CURRENT:
        if (current_tty == TTY_NONE)
            return -1;
        return _read_screen((size_t)current_tty, buf, len);
    case TTY_HANDLE_CONSOLE:
        return tty_input_read(TTY_CONSOLE, buf, len);
    case TTY_HANDLE_NAMED:
        if (handle->index >= TTY_COUNT)
            return -1;
        return _read_screen(TTY_USER_TO_SCREEN(handle->index), buf, len);
    default:
        return -1;
    }
}

ssize_t tty_write_handle(const tty_handle_t* handle, const void* buf, size_t len) {
    if (!handle)
        return -1;

    switch (handle->kind) {
    case TTY_HANDLE_CURRENT:
        if (current_tty == TTY_NONE)
            return -1;
        return _write_screen_processed((size_t)current_tty, buf, len);
    case TTY_HANDLE_CONSOLE:
        return _write_screen_processed(TTY_CONSOLE, buf, len);
    case TTY_HANDLE_NAMED:
        if (handle->index >= TTY_COUNT)
            return -1;
        return _write_screen_processed(TTY_USER_TO_SCREEN(handle->index), buf, len);
    default:
        return -1;
    }
}

ssize_t tty_ioctl_handle(const tty_handle_t* handle, u64 request, void* args) {
    size_t screen = 0;
    if (!_resolve_screen(handle, &screen))
        return -1;

    switch (request) {
    case TIOCGWINSZ:
        if (!args)
            return -1;
        return tty_input_get_winsize(screen, args) ? 0 : -1;
    case TIOCSWINSZ:
        if (!args)
            return -1;
        return tty_input_set_winsize(screen, args) ? 0 : -1;
    case TCGETS:
        if (!args)
            return -1;
        return tty_input_get_termios(screen, args) ? 0 : -1;
    case TCSETS:
        if (!args)
            return -1;
        return tty_input_set_termios(screen, args, false) ? 0 : -1;
    case TCSETSW:
    case TCSETSF:
        if (!args)
            return -1;
        return tty_input_set_termios(screen, args, true) ? 0 : -1;
    case TIOCSPGRP:
        if (!args)
            return -1;
        tty_pgrp[screen] = *(pid_t*)args;
        return 0;
    case TIOCGPGRP:
        if (!args)
            return -1;
        *(pid_t*)args = tty_pgrp[screen];
        return 0;
    default:
        return -1;
    }
}
