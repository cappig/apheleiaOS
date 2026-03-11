#include "wm_file.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

bool wm_file_read_all(
    const char *path,
    size_t max_bytes,
    u8 **data_out,
    size_t *len_out
) {
    if (!path || !data_out || !len_out || !max_bytes) {
        return false;
    }

    *data_out = NULL;
    *len_out = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    struct stat st = {0};
    if (fstat(fd, &st) < 0) {
        close(fd);
        return false;
    }

    if (st.st_size <= 0 || st.st_size > (off_t)max_bytes) {
        close(fd);
        return false;
    }

    size_t len = (size_t)st.st_size;
    if ((off_t)len != st.st_size) {
        close(fd);
        return false;
    }

    u8 *data = malloc(len);
    if (!data) {
        close(fd);
        return false;
    }

    size_t read_total = 0;
    while (read_total < len) {
        ssize_t n = read(fd, data + read_total, len - read_total);

        if (n < 0) {
            free(data);
            close(fd);
            return false;
        }

        if (!n) {
            break;
        }

        read_total += (size_t)n;
    }

    close(fd);

    if (read_total != len) {
        free(data);
        return false;
    }

    *data_out = data;
    *len_out = len;

    return true;
}
