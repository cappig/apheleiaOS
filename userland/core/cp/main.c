#include <errno.h>
#include <fcntl.h>
#include <fsutil.h>
#include <io.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(void) {
    io_write_str("usage: cp SOURCE... DEST\n");
}

static void print_error(const char *src, const char *dst) {
    char line[320];
    snprintf(
        line,
        sizeof(line),
        "cp: %s -> %s: %s\n",
        src ? src : "(null)",
        dst ? dst : "(null)",
        strerror(errno)
    );
    io_write_str(line);
}

static int copy_fd_data(int in_fd, int out_fd) {
    char buf[4096];

    for (;;) {
        ssize_t read_len = read(in_fd, buf, sizeof(buf));
        if (read_len == 0) {
            return 0;
        }

        if (read_len < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        size_t off = 0;
        while (off < (size_t)read_len) {
            ssize_t wrote = write(out_fd, buf + off, (size_t)read_len - off);
            if (wrote < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            off += (size_t)wrote;
        }
    }
}

static int copy_file(const char *src, const char *dst) {
    struct stat st_src = {0};
    if (lstat(src, &st_src) < 0) {
        return -1;
    }

    if (fs_is_dir_mode(st_src.st_mode)) {
        errno = EISDIR;
        return -1;
    }

    struct stat st_dst = {0};
    if (!lstat(dst, &st_dst)) {
        if (st_src.st_dev == st_dst.st_dev && st_src.st_ino == st_dst.st_ino) {
            errno = EINVAL;
            return -1;
        }
    }

    int in_fd = open(src, O_RDONLY, 0);
    if (in_fd < 0) {
        return -1;
    }

    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st_src.st_mode & 07777);
    if (out_fd < 0) {
        int saved = errno;
        close(in_fd);
        errno = saved;
        return -1;
    }

    int rc = copy_fd_data(in_fd, out_fd);
    int saved = errno;

    close(out_fd);
    close(in_fd);

    if (rc < 0) {
        errno = saved;
        return -1;
    }

    if (chmod(dst, st_src.st_mode & 07777) < 0) {
        return -1;
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage();
        return 1;
    }

    const char *dest = argv[argc - 1];
    struct stat st_dest = {0};

    bool dest_exists = (!lstat(dest, &st_dest));
    bool dest_is_dir = dest_exists && fs_is_dir_mode(st_dest.st_mode);

    if (argc > 3 && !dest_is_dir) {
        io_write_str("cp: destination is not a directory\n");
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc - 1; i++) {
        const char *src = argv[i];
        char target[PATH_MAX];

        if (dest_is_dir) {
            fs_join_path(target, sizeof(target), dest, fs_path_basename(src));
        } else {
            snprintf(target, sizeof(target), "%s", dest);
        }

        if (copy_file(src, target) < 0) {
            print_error(src, target);
            rc = 1;
        }
    }

    return rc;
}
