#include <errno.h>
#include <io.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void print_error(const char* target, const char* link_path) {
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

int main(int argc, char** argv) {
    bool force = false;
    int argi = 1;

    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "--")) {
            argi++;
            break;
        }

        if (!strcmp(argv[argi], "-f")) {
            force = true;
            argi++;
            continue;
        }

        io_write_str("usage: ln [-f] TARGET LINK_NAME\n");
        return 1;
    }

    if (argc - argi != 2) {
        io_write_str("usage: ln [-f] TARGET LINK_NAME\n");
        return 1;
    }

    const char* target = argv[argi];
    const char* link_path = argv[argi + 1];

    if (!link(target, link_path))
        return 0;

    if (force && errno == EEXIST) {
        if (!unlink(link_path) && !link(target, link_path))
            return 0;
    }

    print_error(target, link_path);
    return 1;
}
