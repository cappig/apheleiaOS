#include <sys/wait.h>
#include <unistd.h>


pid_t waitpid(pid_t pid, int* status, int options) {
    return syscall3(SYS_WAIT, pid, (u64)status, options);
}

pid_t wait(int* status) {
    return waitpid(-1, status, 0);
}
