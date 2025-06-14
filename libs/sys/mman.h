#pragma once

#include <stddef.h>
#include <sys/types.h>

#define MAP_FAILED ((void*)(-1))

#define PROT_NONE  0
#define PROT_READ  (1 << 0)
#define PROT_WRITE (1 << 1)
#define PROT_EXEC  (1 << 2)

#define MAP_SHARED    (1 << 0)
#define MAP_PRIVATE   (1 << 1)
#define MAP_FIXED     (1 << 2)
#define MAP_ANONYMOUS (1 << 3)
#define MAP_POPULATE  (1 << 4)


void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void* addr, size_t length);
