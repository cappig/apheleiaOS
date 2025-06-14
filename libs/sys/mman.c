#include <sys/mman.h>
#include <unistd.h>


void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) {
    return (void*)syscall6(SYS_MMAP, (u64)addr, length, prot, flags, fd, offset);
}
