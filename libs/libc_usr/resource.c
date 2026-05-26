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

    proc_stat_t stat = { 0 };
    if (proc_stat_read_path("/proc/self/stat", &stat) < 0) {
        return -1;
    }

    uint64_t user_time_ms = stat.user_time_ms;
    uint64_t sys_time_ms = stat.sys_time_ms;

    if (who == RUSAGE_CHILDREN) {
        user_time_ms = stat.child_user_time_ms;
        sys_time_ms = stat.child_sys_time_ms;
    }

    usage->ru_utime.tv_sec = (time_t)(user_time_ms / 1000ULL);
    usage->ru_utime.tv_usec = (suseconds_t)((user_time_ms % 1000ULL) * 1000ULL);
    usage->ru_stime.tv_sec = (time_t)(sys_time_ms / 1000ULL);
    usage->ru_stime.tv_usec = (suseconds_t)((sys_time_ms % 1000ULL) * 1000ULL);

    return 0;
}
