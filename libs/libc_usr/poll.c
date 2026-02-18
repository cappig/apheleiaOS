#include <arch/sys.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>

int poll(struct pollfd *fds, nfds_t nfds, int timeout_ms) {
    return (int)__SYSCALL_ERRNO(
        syscall3(SYS_POLL, (uintptr_t)fds, (uintptr_t)nfds, (uintptr_t)timeout_ms)
    );
}
