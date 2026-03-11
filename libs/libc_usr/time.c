#include <apheleia/syscall.h>
#include <arch/sys.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>

static int syscall_sleep(const struct timespec *req, struct timespec *rem) {
    if (!req) {
        errno = EINVAL;
        return -1;
    }

    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }

    return (int)__SYSCALL_ERRNO(
        syscall2(SYS_SLEEP, (uintptr_t)req, (uintptr_t)rem)
    );
}

static int syscall_time(struct timespec *realtime, struct timespec *monotonic) {
    long ret =
        (long)syscall2(SYS_TIME, (uintptr_t)realtime, (uintptr_t)monotonic);

    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }

    return 0;
}

int clock_gettime(clockid_t clock_id, struct timespec *tp) {
    if (!tp) {
        errno = EINVAL;
        return -1;
    }

    if (clock_id == CLOCK_REALTIME) {
        return syscall_time(tp, NULL);
    }

    if (clock_id == CLOCK_MONOTONIC) {
        return syscall_time(NULL, tp);
    }

    errno = EINVAL;
    return -1;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return syscall_sleep(req, rem);
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
