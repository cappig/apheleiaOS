#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        write(STDERR_FILENO, "usage: sleep SECONDS\n", 21);
        return 1;
    }

    int seconds = atoi(argv[1]);
    if (seconds < 0) {
        return 0;
    }

    sleep((unsigned int)seconds);
    return 0;
}
