#pragma once

#include <sys/types.h>

typedef unsigned int nfds_t;

typedef struct pollfd {
    int fd;
    short events;
    short revents;
} pollfd_t;

#define POLLIN  0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

#ifndef _KERNEL
int poll(struct pollfd* fds, nfds_t nfds, int timeout_ms);
#endif
