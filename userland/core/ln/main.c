#include <errno.h>
#include <io.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void print_error(const char *target, const char *link_path) {
    char line[320];
    snprintf(
        line,
        sizeof(line),
        "ln: %s -> %s: %d\n",
        target ? target : "(null)",
        link_path ? link_path : "(null)",
        errno
    );
    io_write_str(line);
}

static void usage(void) {
    io_write_str("usage: ln [-f] [-s] TARGET LINK_NAME\n");
}

int main(int argc, char **argv) {
    bool force = false;
    bool symbolic = false;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-' && argv[argi][1]) {
        const char *arg = argv[argi];

        if (!strcmp(arg, "--")) {
            argi++;
            break;
        }

        for (size_t i = 1; arg[i]; i++) {
            if (arg[i] == 'f') {
                force = true;
            } else if (arg[i] == 's') {
                symbolic = true;
            } else {
                usage();
                return 1;
            }
        }

        argi++;
    }

    if (argc - argi != 2) {
        usage();
        return 1;
    }

    const char *target = argv[argi];
    const char *link_path = argv[argi + 1];

    int rc = symbolic ? symlink(target, link_path) : link(target, link_path);
    if (!rc) {
        return 0;
    }

    if (force && errno == EEXIST) {
        if (!unlink(link_path)) {
            rc = symbolic ? symlink(target, link_path) : link(target, link_path);
        }

        if (!rc) {
            return 0;
        }
    }

    print_error(target, link_path);
    return 1;
}
