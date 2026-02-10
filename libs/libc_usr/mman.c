#include <arch/sys.h>
#include <sys/mman.h>
#include <unistd.h>

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t offset) {
    mmap_args_t args = {
        .addr = addr,
        .len = len,
        .prot = prot,
        .flags = flags,
        .fd = fd,
        .offset = offset,
    };

    return (void*)syscall1(SYS_MMAP, (uintptr_t)&args);
}

int munmap(void* addr, size_t len) {
    return (int)syscall2(SYS_MUNMAP, (uintptr_t)addr, (uintptr_t)len);
}
