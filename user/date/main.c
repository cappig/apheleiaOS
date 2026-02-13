#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int usage(void) {
    const char* msg = "usage: date [+FORMAT]\n";
    write(STDOUT_FILENO, msg, strlen(msg));
    return 1;
}

int main(int argc, char** argv) {
    const char* format = "%a %b %e %H:%M:%S UTC %Y";

    if (argc > 2)
        return usage();

    if (argc == 2) {
        if (argv[1][0] != '+')
            return usage();

        format = argv[1] + 1;
    }

    time_t now = time(NULL);

    if (now < 0) {
        const char* msg = "date: failed to read clock\n";
        write(STDOUT_FILENO, msg, strlen(msg));
        return 1;
    }

    struct tm tm_val = {0};

    if (!gmtime_r(&now, &tm_val)) {
        const char* msg = "date: failed to convert time\n";
        write(STDOUT_FILENO, msg, strlen(msg));
        return 1;
    }

    char out[256] = {0};

    if (format[0] && !strftime(out, sizeof(out), format, &tm_val)) {
        const char* msg = "date: format too long\n";
        write(STDOUT_FILENO, msg, strlen(msg));
        return 1;
    }

    write(STDOUT_FILENO, out, strlen(out));
    write(STDOUT_FILENO, "\n", 1);

    return 0;
}
