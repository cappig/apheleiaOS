#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "unknown"
#endif

static void read_machine(char *out, size_t out_len) {
    if (!out || !out_len) {
        return;
    }

    int fd = open("/dev/os", O_RDONLY, 0);
    if (fd < 0) {
        return;
    }

    char info[256];
    ssize_t len = read(fd, info, sizeof(info) - 1);
    close(fd);

    if (len <= 0) {
        return;
    }

    info[len] = '\0';

    const char key[] = "arch=";
    char *value = strstr(info, key);
    if (!value || (value != info && value[-1] != '\n')) {
        return;
    }

    value += sizeof(key) - 1;
    char *end = strchr(value, '\n');
    size_t value_len = end ? (size_t)(end - value) : strlen(value);
    if (value_len >= out_len) {
        value_len = out_len - 1;
    }

    memcpy(out, value, value_len);
    out[value_len] = '\0';
}

int uname(struct utsname *buf) {
    if (!buf) {
        errno = EINVAL;
        return -1;
    }

    memset(buf, 0, sizeof(*buf));
    strncpy(buf->sysname, "apheleiaOS", sizeof(buf->sysname) - 1);
    strncpy(buf->nodename, "apheleia", sizeof(buf->nodename) - 1);
    strncpy(buf->release, VERSION, sizeof(buf->release) - 1);
    strncpy(buf->version, "posix-base", sizeof(buf->version) - 1);
    strncpy(buf->machine, "unknown", sizeof(buf->machine) - 1);
    read_machine(buf->machine, sizeof(buf->machine));

    return 0;
}
