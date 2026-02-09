#include "tty.h"

#include <arch/arch.h>
#include <sys/tty_input.h>

static ssize_t current_tty = TTY_NONE;

void tty_init(void) {
    if (TTY_SCREEN_COUNT == 0)
        return;

    size_t initial = (TTY_SCREEN_COUNT > TTY_FIRST_USER) ? TTY_FIRST_USER : TTY_CONSOLE;
    current_tty = (ssize_t)initial;
    tty_input_init();
    tty_input_set_current((size_t)current_tty);
    arch_console_set_active((size_t)current_tty);
}

bool tty_set_current(size_t index) {
    if (index >= TTY_SCREEN_COUNT)
        return false;

    current_tty = (ssize_t)index;
    tty_input_set_current(index);
    arch_console_set_active(index);
    return true;
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
        return _write_screen((size_t)current_tty, buf, len);
    case TTY_HANDLE_CONSOLE:
        return _write_screen(TTY_CONSOLE, buf, len);
    case TTY_HANDLE_NAMED:
        if (handle->index >= TTY_COUNT)
            return -1;
        return _write_screen(TTY_USER_TO_SCREEN(handle->index), buf, len);
    default:
        return -1;
    }
}
