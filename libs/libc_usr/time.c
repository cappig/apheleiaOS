#include <apheleia/syscall.h>
#include <arch/sys.h>
#include <errno.h>
#include <fcntl.h>
#include <kv.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int clock_fd = -1;

static bool read_clock_text(char *text, size_t text_len) {
    if (!text || text_len < 2) {
        return false;
    }

    if (clock_fd < 0) {
        clock_fd = open("/dev/clock", O_RDONLY, 0);
        if (clock_fd < 0) {
            return false;
        }
    }

    if (lseek(clock_fd, 0, SEEK_SET) < 0) {
        close(clock_fd);
        clock_fd = -1;
        return false;
    }

    return kv_read_fd(clock_fd, text, text_len) > 0;
}

int clock_gettime(clockid_t clock_id, struct timespec *tp) {
    if (!tp) {
        errno = EINVAL;
        return -1;
    }

    char text[256] = {0};
    if (!read_clock_text(text, sizeof(text))) {
        errno = EIO;
        return -1;
    }

    if (clock_id == CLOCK_REALTIME) {
        unsigned long long now = 0;
        if (!kv_read_u64(text, "now", &now)) {
            errno = EIO;
            return -1;
        }

        tp->tv_sec = (time_t)now;
        tp->tv_nsec = 0;
        return 0;
    }

    if (clock_id == CLOCK_MONOTONIC) {
        unsigned long long hz = 0;
        unsigned long long ticks = 0;

        if (!kv_read_u64(text, "hz", &hz) || !kv_read_u64(text, "ticks", &ticks) || !hz) {
            errno = EIO;
            return -1;
        }

        tp->tv_sec = (time_t)(ticks / hz);
        tp->tv_nsec = (long)(((ticks % hz) * 1000000000ULL) / hz);
        return 0;
    }

    errno = EINVAL;
    return -1;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return (int)__SYSCALL_ERRNO(
        syscall2(SYS_SLEEP, (uintptr_t)req, (uintptr_t)rem)
    );
}

time_t time(time_t *timer) {
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        return (time_t)-1;
    }

    if (timer) {
        *timer = ts.tv_sec;
    }

    return ts.tv_sec;
}

int gettimeofday(struct timeval *tv, struct timezone *tz) {
    if (!tv) {
        errno = EINVAL;
        return -1;
    }

    struct timespec ts = {0};
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        return -1;
    }

    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = (suseconds_t)(ts.tv_nsec / 1000L);

    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }

    return 0;
}
