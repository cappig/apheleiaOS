#include <string.h>
#include <unistd.h>

#ifndef ARCH_NAME
#define ARCH_NAME "unknown"
#endif

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    const char out[] = "apheleiaOS " ARCH_NAME "\n";

    write(STDOUT_FILENO, out, sizeof(out) - 1);
    return 0;
}
