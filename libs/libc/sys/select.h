#pragma once

#include <stddef.h>
#include <sys/time.h>

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

typedef struct fd_set {
    unsigned long fds_bits[(FD_SETSIZE + (8 * sizeof(unsigned long)) - 1) / (8 * sizeof(unsigned long))];
} fd_set;

static inline int fdset_ok(int fd) {
    return fd >= 0 && fd < FD_SETSIZE;
}

static inline size_t fdset_word(int fd) {
    return (size_t)fd / (8 * sizeof(unsigned long));
}

static inline unsigned long fdset_mask(int fd) {
    return 1UL << ((size_t)fd % (8 * sizeof(unsigned long)));
}

static inline void fdset_zero(fd_set *set) {
    for (size_t i = 0; i < sizeof(set->fds_bits) / sizeof(set->fds_bits[0]); i++) {
        set->fds_bits[i] = 0;
    }
}

static inline void fdset_add(int fd, fd_set *set) {
    if (fdset_ok(fd)) {
        set->fds_bits[fdset_word(fd)] |= fdset_mask(fd);
    }
}

static inline void fdset_del(int fd, fd_set *set) {
    if (fdset_ok(fd)) {
        set->fds_bits[fdset_word(fd)] &= ~fdset_mask(fd);
    }
}

static inline int fdset_has(int fd, const fd_set *set) {
    if (!fdset_ok(fd)) {
        return 0;
    }

    return (set->fds_bits[fdset_word(fd)] & fdset_mask(fd)) != 0;
}

#define FD_ZERO(set)      fdset_zero(set)
#define FD_SET(fd, set)   fdset_add((fd), (set))
#define FD_CLR(fd, set)   fdset_del((fd), (set))
#define FD_ISSET(fd, set) fdset_has((fd), (set))

#ifndef _KERNEL
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
#endif
