#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    const char seq[] = "\x1b[2J\x1b[H";
    write(STDOUT_FILENO, seq, sizeof(seq) - 1);
    return 0;
}
