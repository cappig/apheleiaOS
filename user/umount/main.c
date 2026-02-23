#include <errno.h>
#include <io.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

static void usage(void) {
    io_write_str("usage: umount TARGET\n");
}

int main(int argc, char **argv) {
    if (argc != 2) {
        usage();
        return 1;
    }

    const char *target = argv[1];

    if (umount(target, 0) < 0) {
        fprintf(
            stderr,
            "umount: failed to unmount '%s': %s\n",
            target,
            strerror(errno)
        );
        return 1;
    }

    return 0;
}
