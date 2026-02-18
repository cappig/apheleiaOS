#pragma once

#include <base/types.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/proc.h>
#include <sys/types.h>

#define TTY_COUNT        4
#define TTY_SCREEN_COUNT (TTY_COUNT + 1)
#define TTY_CONSOLE      0
#define TTY_FIRST_USER   1
#define TTY_NONE         PROC_TTY_NONE

#define TTY_USER_TO_SCREEN(index) ((index) + TTY_FIRST_USER)
#define TTY_SCREEN_TO_USER(index) ((index) - TTY_FIRST_USER)

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
pid_t tty_get_pgrp(size_t index);
size_t tty_current_screen(void);

ssize_t tty_read_handle(const tty_handle_t *handle, void *buf, size_t len);
ssize_t tty_write_handle(const tty_handle_t *handle, const void *buf, size_t len);
ssize_t tty_ioctl_handle(const tty_handle_t *handle, u64 request, void *args);
short tty_poll_handle(const tty_handle_t *handle, short events, u32 flags);
ssize_t tty_write_screen_output(size_t index, const void *buf, size_t len);
