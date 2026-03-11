#include <errno.h>
#include <string.h>
#include <sys/utsname.h>

int uname(struct utsname *buf) {
    if (!buf) {
        errno = EINVAL;
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    strncpy(buf->sysname, "apheleiaOS", sizeof(buf->sysname) - 1);
    strncpy(buf->nodename, "apheleia", sizeof(buf->nodename) - 1);
    strncpy(buf->release, "alpha", sizeof(buf->release) - 1);
    strncpy(buf->version, "posix-base", sizeof(buf->version) - 1);
#ifdef ARCH_NAME
    strncpy(buf->machine, ARCH_NAME, sizeof(buf->machine) - 1);
#else
    strncpy(buf->machine, "unknown", sizeof(buf->machine) - 1);
#endif

    return 0;
}
