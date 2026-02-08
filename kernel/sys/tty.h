#pragma once

#include <base/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define TTY_COUNT   4
#define TTY_CONSOLE (TTY_COUNT - 1)
#define TTY_NONE    (-1)

typedef enum {
    TTY_HANDLE_CURRENT,
    TTY_HANDLE_CONSOLE,
    TTY_HANDLE_NAMED,
} tty_handle_kind_t;

typedef struct {
    tty_handle_kind_t kind;
    size_t index;
} tty_handle_t;

void tty_init(void);
bool tty_set_current(size_t index);

ssize_t tty_read_handle(const tty_handle_t* handle, void* buf, size_t len);
ssize_t tty_write_handle(const tty_handle_t* handle, const void* buf, size_t len);
