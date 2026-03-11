#include <limits.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    char cwd[PATH_MAX];

    if (getcwd(cwd, sizeof(cwd))) {
        write(STDOUT_FILENO, cwd, strlen(cwd));
        write(STDOUT_FILENO, "\n", 1);
    } else {
        write(STDOUT_FILENO, "/\n", 2);
    }

    return 0;
}
