#include <stdio.h>
#include <string.h>
#include <unistd.h>

char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;

static int opt_pos = 1;

static int _getopt_error(const char *prog, const char *msg, int opt) {
    optopt = opt;

    if (opterr && msg) {
        if (prog && prog[0]) {
            fprintf(stderr, "%s: ", prog);
        }
        fprintf(stderr, "%s -- %c\n", msg, opt);
    }

    return '?';
}

int getopt(int argc, char *const argv[], const char *optstring) {
    optarg = NULL;

    if (!optstring || optind < 1) {
        optind = 1;
        opt_pos = 1;
    }

    if (optind >= argc || !argv || !argv[optind]) {
        return -1;
    }

    const char *arg = argv[optind];
    if (opt_pos == 1) {
        if (arg[0] != '-' || arg[1] == '\0') {
            return -1;
        }

        if (!strcmp(arg, "--")) {
            optind++;
            return -1;
        }
    }

    int opt = (unsigned char)arg[opt_pos++];
    const char *spec = strchr(optstring, opt);
    if (!spec || opt == ':') {
        if (arg[opt_pos] == '\0') {
            optind++;
            opt_pos = 1;
        }

        return _getopt_error(argv[0], "illegal option", opt);
    }

    if (spec[1] == ':') {
        if (arg[opt_pos] != '\0') {
            optarg = (char *)&arg[opt_pos];
            optind++;
            opt_pos = 1;
        } else if (optind + 1 < argc) {
            optarg = argv[++optind];
            optind++;
            opt_pos = 1;
        } else {
            optind++;
            opt_pos = 1;
            optopt = opt;

            if (optstring[0] == ':') {
                return ':';
            }

            return _getopt_error(argv[0], "option requires an argument", opt);
        }
    } else if (arg[opt_pos] == '\0') {
        optind++;
        opt_pos = 1;
    }

    return opt;
}
