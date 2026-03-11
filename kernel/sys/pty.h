#pragma once

#include <base/types.h>
#include <poll.h>
#include <stddef.h>
#include <sys/types.h>
#include <termios.h>

#define PTY_COUNT       8
#define PTY_BUFFER_SIZE 4096

typedef struct {
    size_t index;
    bool is_master;
} pty_handle_t;

void pty_init(void);

bool pty_reserve(size_t *index_out);
void pty_unreserve(size_t index);
void pty_hold(size_t index);
void pty_put(size_t index);

ssize_t
pty_read_handle(const pty_handle_t *handle, void *buf, size_t len, u32 flags);
ssize_t pty_write_handle(
    const pty_handle_t *handle,
    const void *buf,
    size_t len,
    u32 flags
);
ssize_t pty_ioctl_handle(const pty_handle_t *handle, u64 request, void *args);
short pty_poll_handle(const pty_handle_t *handle, short events, u32 flags);
