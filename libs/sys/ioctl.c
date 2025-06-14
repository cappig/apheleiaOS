#include <sys/ioctl.h>
#include <unistd.h>


int ioctl(int fd, unsigned long request, struct ioctl* data) {
    return syscall3(SYS_IOCTL, fd, request, (u64)data);
}
