#include <sys/types.h>
#include <unistd.h>
#include <string.h>

static void write_str(const char* str) {
    if (!str)
        return;

    write(STDOUT_FILENO, str, strlen(str));
}

int main(void) {
    write_str("init: starting /sbin/sh.elf\n");

    for (;;) {
        pid_t pid = fork();

        if (pid == 0) {
            if (execve("/sbin/sh.elf", NULL, NULL) < 0) {
                write_str("init: exec failed\n");
                _exit(1);
            }
        }

        if (pid < 0) {
            write_str("init: fork failed\n");
            continue;
        }

        int status = 0;
        wait(pid, &status);
    }

    return 0;
}
