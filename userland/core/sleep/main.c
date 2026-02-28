#include <io.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        io_write_str("usage: sleep SECONDS\n");
        return 1;
    }

    int seconds = atoi(argv[1]);
    if (seconds < 0) {
        return 0;
    }

    sleep((unsigned int)seconds);

    return 0;
}
