#include <apheleia/syscall.h>
#include <arch/sys.h>
#include <errno.h>
#include <kv.h>
#include <stdint.h>
#include <time.h>

int clock_gettime(clockid_t clock_id, struct timespec *tp) {
    if (!tp) {
        errno = EINVAL;
        return -1;
    }

    char text[256] = {0};
    if (kv_read_file("/dev/clock", text, sizeof(text)) <= 0) {
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
