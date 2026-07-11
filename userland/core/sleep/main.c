#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

static bool parse_seconds(const char *text, unsigned int *out) {
    if (!text || !*text || !out || *text == '-') {
        return false;
    }

    errno = 0;
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end || value > UINT_MAX) {
        return false;
    }

    *out = (unsigned int)value;
    return true;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        static const char usage[] = "usage: sleep SECONDS\n";
        write(STDERR_FILENO, usage, sizeof(usage) - 1);
        return 1;
    }

    unsigned int seconds = 0;
    if (!parse_seconds(argv[1], &seconds)) {
        static const char error[] = "sleep: invalid duration\n";
        write(STDERR_FILENO, error, sizeof(error) - 1);
        return 1;
    }

    return sleep(seconds) ? 1 : 0;
}
