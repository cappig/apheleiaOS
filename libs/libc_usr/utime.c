#include <errno.h>
#include <utime.h>

int utime(const char *filename, const struct utimbuf *times) {
    (void)filename;
    (void)times;
    errno = ENOSYS;
    return -1;
}
