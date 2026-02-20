#include <errno.h>
#include <fsutil.h>
#include <io.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void print_error(const char *src, const char *dst) {
    char line[320];
    snprintf(
        line, sizeof(line), "mv: %s -> %s: %d\n", src ? src : "(null)", dst ? dst : "(null)", errno
    );
    io_write_str(line);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        io_write_str("usage: mv SOURCE... DEST\n");
        return 1;
    }

    const char *dest = argv[argc - 1];
    struct stat st_dest = {0};

    bool dest_exists = (!stat(dest, &st_dest));
    bool dest_is_dir = dest_exists && fs_is_dir_mode(st_dest.st_mode);

    if (argc > 3 && !dest_is_dir) {
        io_write_str("mv: destination is not a directory\n");
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

        if (rename(src, target) < 0) {
            print_error(src, target);
            rc = 1;
        }
    }

    return rc;
}
