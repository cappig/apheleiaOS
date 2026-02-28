#include <errno.h>
#include <io.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>

static void usage(void) {
    io_write_str("usage: mount [-t FSTYPE] SOURCE TARGET\n");
    io_write_str("       mount SOURCE TARGET [-t FSTYPE]\n");
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage();
        return 1;
    }

    const char *source = NULL;
    const char *target = NULL;
    const char *fstype = "ext2";

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!strcmp(arg, "-t")) {
            if (++i >= argc || !argv[i] || !argv[i][0]) {
                usage();
                return 1;
            }

            fstype = argv[i];
            continue;
        }

        if (arg[0] == '-') {
            usage();
            return 1;
        }

        if (!source) {
            source = arg;
            continue;
        }

        if (!target) {
            target = arg;
            continue;
        }

        usage();
        return 1;
    }

    if (!source || !target) {
        usage();
        return 1;
    }

    if (mount(source, target, fstype, 0) < 0) {
        fprintf(
            stderr,
            "mount: failed to mount '%s' on '%s' (%s): %s\n",
            source,
            target,
            fstype,
            strerror(errno)
        );
        return 1;
    }

    return 0;
}
