#include "psf.h"

#include <fcntl.h>
#include <unistd.h>

bool psf_parse(const void *data, size_t len, psf_font_t *out) {
    return psf_parse_blob(data, len, out);
}

bool psf_load_file(const char *path, void *storage, size_t storage_len, psf_font_t *out) {
    if (!path || !path[0] || !storage || !storage_len || !out) {
        return false;
    }

    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        return false;
    }

    ssize_t n = read(fd, storage, storage_len);
    close(fd);

    if (n <= 0) {
        return false;
    }

    return psf_parse_blob(storage, (size_t)n, out);
}
