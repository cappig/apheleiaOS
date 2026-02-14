#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int parse_signal(const char* arg) {
    if (!arg || !*arg)
        return -1;

    if (arg[0] == '-') {
        arg++;
        if (!*arg)
            return -1;
    }

    int sig = atoi(arg);
    if (sig > 0)
        return sig;

    return -1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        write(STDOUT_FILENO, "usage: kill [-s SIG] pid\n", 25);
        return 1;
    }

    int sig = SIGTERM;
    int argi = 1;

    if (!strcmp(argv[argi], "-s") && argi + 1 < argc) {
        sig = parse_signal(argv[argi + 1]);
        argi += 2;
    } else if (argv[argi][0] == '-') {
        sig = parse_signal(argv[argi]);
        argi += 1;
    }

    if (sig <= 0 || argi >= argc) {
        write(STDOUT_FILENO, "kill: invalid signal\n", 21);
        return 1;
    }

    pid_t pid = (pid_t)atoi(argv[argi]);
    if (pid <= 0) {
        write(STDOUT_FILENO, "kill: invalid pid\n", 18);
        return 1;
    }

    if (kill(pid, sig) < 0) {
        write(STDOUT_FILENO, "kill: failed\n", 13);
        return 1;
    }

    return 0;
}
