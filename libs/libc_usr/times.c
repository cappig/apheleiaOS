#include <errno.h>
#include <stdbool.h>
#include <sys/proc.h>
#include <sys/times.h>
#include <time.h>

#define TIMES_HZ 100

clock_t times(struct tms *buf) {
    if (buf) {
        proc_stat_t stat = {0};
        if (proc_stat_read_path("/proc/self/stat", &stat) >= 0) {
            clock_t user_ticks =
                (clock_t)((stat.cpu_time_ms * TIMES_HZ) / 1000ULL);
            buf->tms_utime = user_ticks;
            buf->tms_stime = 0;
            buf->tms_cutime = 0;
            buf->tms_cstime = 0;
        } else {
            buf->tms_utime = 0;
            buf->tms_stime = 0;
            buf->tms_cutime = 0;
            buf->tms_cstime = 0;
        }
    }

    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        return (clock_t)-1;
    }

    return
        (clock_t)(now.tv_sec * TIMES_HZ) +
        (clock_t)((now.tv_nsec * TIMES_HZ) / 1000000000L);
}
