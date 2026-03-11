#include <apheleia/syscall.h>
#include <arch/sys.h>
#include <errno.h>
#include <fcntl.h>
#include <kv.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static int clock_fd = -1;
static bool clock_ioctl_supported = true;

typedef struct {
    unsigned long long now;
    unsigned long long boot;
    unsigned long long hz;
    unsigned long long ticks;
    unsigned long long monotonic_ns;
} clock_snapshot_t;

static bool ensure_clock_fd(void) {
    if (clock_fd >= 0) {
        return true;
    }

    clock_fd = open("/dev/clock", O_RDONLY, 0);
    return clock_fd >= 0;
}

static bool read_clock_text(char *text, size_t text_len) {
    if (!text || text_len < 2) {
        return false;
    }

    if (!ensure_clock_fd()) {
        return false;
    }

    if (lseek(clock_fd, 0, SEEK_SET) < 0) {
        close(clock_fd);
        clock_fd = -1;
        return false;
    }

    return kv_read_fd(clock_fd, text, text_len) > 0;
}

static bool read_clock_snapshot(clock_snapshot_t *out) {
    if (!out) {
        return false;
    }

    if (clock_ioctl_supported && ensure_clock_fd()) {
        if (ioctl(clock_fd, CLOCKIO_GETSNAPSHOT, out) == 0) {
            return true;
        }

        if (errno == ENOTTY || errno == EINVAL) {
            clock_ioctl_supported = false;
        } else {
            close(clock_fd);
            clock_fd = -1;
        }
    }

    char text[256] = {0};
    if (!read_clock_text(text, sizeof(text))) {
        return false;
    }

    bool ok = true;
    ok &= kv_read_u64(text, "now", &out->now);
    ok &= kv_read_u64(text, "boot", &out->boot);
    ok &= kv_read_u64(text, "hz", &out->hz);
    ok &= kv_read_u64(text, "ticks", &out->ticks);
    (void)kv_read_u64(text, "monotonic_ns", &out->monotonic_ns);
    return ok;
}

int clock_gettime(clockid_t clock_id, struct timespec *tp) {
    if (!tp) {
        errno = EINVAL;
        return -1;
    }

    clock_snapshot_t snapshot = {0};
    if (!read_clock_snapshot(&snapshot)) {
        errno = EIO;
        return -1;
    }

    if (clock_id == CLOCK_REALTIME) {
        tp->tv_sec = (time_t)snapshot.now;
        if (snapshot.hz) {
            tp->tv_nsec =
                (long)(((snapshot.ticks % snapshot.hz) * 1000000000ULL) / snapshot.hz);
        } else if (snapshot.monotonic_ns) {
            tp->tv_nsec = (long)(snapshot.monotonic_ns % 1000000000ULL);
        } else {
            tp->tv_nsec = 0;
        }
        return 0;
    }

    if (clock_id == CLOCK_MONOTONIC) {
        if (snapshot.monotonic_ns) {
            tp->tv_sec = (time_t)(snapshot.monotonic_ns / 1000000000ULL);
            tp->tv_nsec = (long)(snapshot.monotonic_ns % 1000000000ULL);
            return 0;
        }

        if (!snapshot.hz) {
            errno = EIO;
            return -1;
        }

        tp->tv_sec = (time_t)(snapshot.ticks / snapshot.hz);
        tp->tv_nsec = (long)(((snapshot.ticks % snapshot.hz) * 1000000000ULL) / snapshot.hz);
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
