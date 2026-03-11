#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int clear_scrollback = 1;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!arg) {
            continue;
        }

        if (!strcmp(arg, "-x") || !strcmp(arg, "--no-scrollback")) {
            clear_scrollback = 0;
            continue;
        }

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            const char usage[] =
                "usage: clear [-x|--no-scrollback]\n"
                "  -x, --no-scrollback  clear screen only\n"
                "  -h, --help           show this help\n";
            write(STDOUT_FILENO, usage, sizeof(usage) - 1);
            return 0;
        }

        const char usage[] = "usage: clear [-x|--no-scrollback]\n";
        write(STDERR_FILENO, usage, sizeof(usage) - 1);
        return 1;
    }

    const char *seq = clear_scrollback ? "\x1b[3J\x1b[2J\x1b[H" : "\x1b[2J\x1b[H";
    write(STDOUT_FILENO, seq, strlen(seq));
    return 0;
}
