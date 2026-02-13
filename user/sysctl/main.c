#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define SYSCTL_VALUE_MAX 128

static const char* default_keys[] = {
    "kern.ostype",
    "kern.osrelease",
    "kern.arch",
    "kern.uptime",
    "kern.proc.count",
    "hw.model",
    "hw.ncpu",
    "hw.clockrate",
    "hw.pagesize",
    "vm.mem.total_kib",
    "vm.mem.free_kib",
};

static int print_key(const char* key, bool value_only) {
    if (!key || !key[0])
        return 1;

    if (strchr(key, '=')) {
        char line[160];
        snprintf(line, sizeof(line), "sysctl: setting '%s' not supported\n", key);
        write(STDOUT_FILENO, line, strlen(line));
        return 1;
    }

    char value[SYSCTL_VALUE_MAX] = {0};
    if (sysctl(key, value, sizeof(value)) < 0) {
        char line[160];
        snprintf(line, sizeof(line), "sysctl: unknown key '%s'\n", key);
        write(STDOUT_FILENO, line, strlen(line));
        return 1;
    }

    char line[192];
    if (value_only)
        snprintf(line, sizeof(line), "%s\n", value);
    else
        snprintf(line, sizeof(line), "%s=%s\n", key, value);

    write(STDOUT_FILENO, line, strlen(line));
    return 0;
}

int main(int argc, char** argv) {
    bool value_only = false;
    int argi = 1;

    if (argc > 1 && !strcmp(argv[1], "-n")) {
        value_only = true;
        argi = 2;
    }

    int rc = 0;

    if (argi >= argc) {
        for (size_t i = 0; i < sizeof(default_keys) / sizeof(default_keys[0]); i++) {
            if (print_key(default_keys[i], value_only) != 0)
                rc = 1;
        }
        return rc;
    }

    for (int i = argi; i < argc; i++) {
        if (print_key(argv[i], value_only) != 0)
            rc = 1;
    }

    return rc;
}
