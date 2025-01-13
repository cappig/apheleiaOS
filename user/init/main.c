#include <aos/syscalls.h>
#include <string.h>

int main(void) {
    char buf[] = "Hello from userland!\n";
    sys_write(STDOUT_FD, buf, strlen(buf));

    return 0;
}
