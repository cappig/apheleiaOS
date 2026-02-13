#include <arch/sys.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

time_t time(time_t* timer) {
    long ret = syscall1(SYS_TIME, (uintptr_t)timer);

    if (ret < 0) {
        errno = (int)-ret;
        return (time_t)-1;
    }

    time_t now = (time_t)ret;

    if (timer)
        *timer = now;

    return now;
}
