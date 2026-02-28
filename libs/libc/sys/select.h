#pragma once

#include <stddef.h>
#include <sys/time.h>

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

typedef struct fd_set {
    unsigned long fds_bits[(FD_SETSIZE + (8 * sizeof(unsigned long)) - 1) /
                           (8 * sizeof(unsigned long))];
} fd_set;

#define __FD_WORD(fd) ((fd) / (int)(8 * sizeof(unsigned long)))
#define __FD_MASK(fd) (1UL << ((fd) % (int)(8 * sizeof(unsigned long))))

#define FD_ZERO(set)                                                      \
    do {                                                                  \
        fd_set *__fdset = (set);                                          \
        for (size_t __i = 0; __i < sizeof(__fdset->fds_bits) / sizeof(__fdset->fds_bits[0]); __i++) { \
            __fdset->fds_bits[__i] = 0;                                   \
        }                                                                 \
    } while (0)

#define FD_SET(fd, set)                                                   \
    do {                                                                  \
        if ((fd) >= 0 && (fd) < FD_SETSIZE) {                             \
            (set)->fds_bits[__FD_WORD(fd)] |= __FD_MASK(fd);             \
        }                                                                 \
    } while (0)

#define FD_CLR(fd, set)                                                   \
    do {                                                                  \
        if ((fd) >= 0 && (fd) < FD_SETSIZE) {                             \
            (set)->fds_bits[__FD_WORD(fd)] &= ~__FD_MASK(fd);            \
        }                                                                 \
    } while (0)

#define FD_ISSET(fd, set)                                                 \
    (((fd) >= 0 && (fd) < FD_SETSIZE)                                    \
         ? (((set)->fds_bits[__FD_WORD(fd)] & __FD_MASK(fd)) != 0)       \
         : 0)

#ifndef _KERNEL
int select(
    int nfds,
    fd_set *readfds,
    fd_set *writefds,
    fd_set *exceptfds,
    struct timeval *timeout
);
#endif
