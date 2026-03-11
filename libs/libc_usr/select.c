#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

typedef struct select_entry {
    int fd;
    short events;
    short revents;
} select_entry_t;

static int _timeval_to_ms(const struct timeval *tv, int *out_ms) {
    if (!out_ms) {
        errno = EINVAL;
        return -1;
    }

    if (!tv) {
        *out_ms = -1;
        return 0;
    }

    if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000) {
        errno = EINVAL;
        return -1;
    }

    long long ms = (long long)tv->tv_sec * 1000LL;
    ms += (long long)(tv->tv_usec / 1000);
    if (tv->tv_usec % 1000) {
        ms++;
    }

    if (ms > 0x7fffffffLL) {
        ms = 0x7fffffffLL;
    }

    *out_ms = (int)ms;
    return 0;
}

int select(
    int nfds,
    fd_set *readfds,
    fd_set *writefds,
    fd_set *exceptfds,
    struct timeval *timeout
) {
    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }

    int timeout_ms = -1;
    if (_timeval_to_ms(timeout, &timeout_ms) < 0) {
        return -1;
    }

    select_entry_t *entries = NULL;
    struct pollfd *pfds = NULL;
    size_t count = 0;

    if (nfds > 0) {
        entries = calloc((size_t)nfds, sizeof(*entries));
        pfds = calloc((size_t)nfds, sizeof(*pfds));
        if (!entries || !pfds) {
            free(entries);
            free(pfds);
            errno = ENOMEM;
            return -1;
        }
    }

    for (int fd = 0; fd < nfds; fd++) {
        short events = 0;
        if (readfds && FD_ISSET(fd, readfds)) {
            events |= POLLIN;
        }
        if (writefds && FD_ISSET(fd, writefds)) {
            events |= POLLOUT;
        }
        if (exceptfds && FD_ISSET(fd, exceptfds)) {
            events |= POLLERR | POLLHUP;
        }

        if (!events) {
            continue;
        }

        entries[count].fd = fd;
        entries[count].events = events;
        pfds[count].fd = fd;
        pfds[count].events = events;
        pfds[count].revents = 0;
        count++;
    }

    if (readfds) {
        FD_ZERO(readfds);
    }
    if (writefds) {
        FD_ZERO(writefds);
    }
    if (exceptfds) {
        FD_ZERO(exceptfds);
    }

    int ready = 0;
    if (!count) {
        ready = poll(NULL, 0, timeout_ms);
    } else {
        ready = poll(pfds, (nfds_t)count, timeout_ms);
    }

    if (ready <= 0) {
        free(entries);
        free(pfds);
        return ready;
    }

    int selected = 0;
    for (size_t i = 0; i < count; i++) {
        short revents = pfds[i].revents;
        int fd = entries[i].fd;
        bool any = false;

        if ((revents & (POLLIN | POLLHUP | POLLERR)) && readfds) {
            FD_SET(fd, readfds);
            any = true;
        }
        if ((revents & POLLOUT) && writefds) {
            FD_SET(fd, writefds);
            any = true;
        }
        if ((revents & (POLLERR | POLLHUP | POLLNVAL)) && exceptfds) {
            FD_SET(fd, exceptfds);
            any = true;
        }

        if (any) {
            selected++;
        }
    }

    free(entries);
    free(pfds);
    return selected;
}
