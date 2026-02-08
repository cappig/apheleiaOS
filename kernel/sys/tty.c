#include "tty.h"

#include <arch/arch.h>

static ssize_t current_tty = TTY_NONE;

void tty_init(void) {
    if (TTY_COUNT == 0)
        return;

    current_tty = TTY_CONSOLE;
}

bool tty_set_current(size_t index) {
    if (index >= TTY_COUNT)
        return false;

    current_tty = (ssize_t)index;
    return true;
}

static ssize_t _read_index(size_t index, void* buf, size_t len) {
    if (index >= TTY_COUNT)
        return -1;

    return arch_tty_read(buf, len);
}

static ssize_t _write_index(size_t index, const void* buf, size_t len) {
    if (index >= TTY_COUNT)
        return -1;

    return arch_tty_write(buf, len);
}

ssize_t tty_read_handle(const tty_handle_t* handle, void* buf, size_t len) {
    if (!handle)
        return -1;

    switch (handle->kind) {
    case TTY_HANDLE_CURRENT:
        if (current_tty == TTY_NONE)
            return -1;
        return _read_index((size_t)current_tty, buf, len);
    case TTY_HANDLE_CONSOLE:
        return arch_console_read(buf, len);
    case TTY_HANDLE_NAMED:
        return _read_index(handle->index, buf, len);
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
        return _write_index((size_t)current_tty, buf, len);
    case TTY_HANDLE_CONSOLE:
        return arch_console_write(buf, len);
    case TTY_HANDLE_NAMED:
        return _write_index(handle->index, buf, len);
    default:
        return -1;
    }
}
