#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(void) {
    write(STDOUT_FILENO, "usage: chmod MODE FILE...\n", 26);
}

static int parse_mode(const char* text, mode_t* out) {
    if (!text || !*text || !out)
        return -1;

    char* end = NULL;
    long value = strtol(text, &end, 8);

    if (!end || *end != '\0')
        return -1;

    if (value < 0 || value > 07777L)
        return -1;

    *out = (mode_t)value;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        usage();
        return 1;
    }

    mode_t mode = 0;
    if (parse_mode(argv[1], &mode) != 0) {
        write(STDOUT_FILENO, "chmod: invalid mode\n", 20);
        return 1;
    }

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        if (chmod(argv[i], mode) != 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "chmod: %s: %d\n", argv[i], errno);
            write(STDOUT_FILENO, msg, strlen(msg));
            rc = 1;
        }
    }

    return rc;
}
