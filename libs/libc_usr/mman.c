#include <arch/sys.h>
#include <apheleia/syscall.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset) {
    mmap_args_t args = {
        .addr = addr,
        .len = len,
        .prot = prot,
        .flags = flags,
        .fd = fd,
        .offset = offset,
    };

    long ret = syscall1(SYS_MMAP, (uintptr_t)&args);
    if (ret < 0) {
        errno = (int)-ret;
        return MAP_FAILED;
    }
    return (void *)ret;
}

int munmap(void *addr, size_t len) {
    return (int)__SYSCALL_ERRNO(syscall2(SYS_MUNMAP, (uintptr_t)addr, (uintptr_t)len));
}
