#include <io.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int usage(void) {
    io_write_str("usage: date [+FORMAT]\n");
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
        io_write_str("date: failed to read clock\n");
        return 1;
    }

    struct tm tm_val = {0};

    if (!gmtime_r(&now, &tm_val)) {
        io_write_str("date: failed to convert time\n");
        return 1;
    }

    char out[256] = {0};

    if (format[0] && !strftime(out, sizeof(out), format, &tm_val)) {
        io_write_str("date: format too long\n");
        return 1;
    }

    io_write_str(out);
    io_write_str("\n");

    return 0;
}
