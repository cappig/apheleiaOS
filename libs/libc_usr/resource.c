#include <errno.h>
#include <sys/proc.h>
#include <sys/resource.h>

int getrusage(int who, struct rusage *usage) {
    if (!usage) {
        errno = EINVAL;
        return -1;
    }

    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN) {
        errno = EINVAL;
        return -1;
    }

    usage->ru_utime.tv_sec = 0;
    usage->ru_utime.tv_usec = 0;
    usage->ru_stime.tv_sec = 0;
    usage->ru_stime.tv_usec = 0;

    if (who == RUSAGE_CHILDREN) {
        return 0;
    }

    proc_stat_t stat = {0};
    if (proc_stat_read_path("/proc/self/stat", &stat) < 0) {
        return -1;
    }

    usage->ru_utime.tv_sec = (time_t)(stat.cpu_time_ms / 1000ULL);
    usage->ru_utime.tv_usec = (suseconds_t)((stat.cpu_time_ms % 1000ULL) * 1000ULL);

    return 0;
}
