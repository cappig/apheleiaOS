#pragma once

#include <sys/types.h>

#define WNOHANG   (1 << 0)
#define WUNTRACED (1 << 1)

#define WIFSTOPPED(status) (((status) & 0xff) == 0x7f)
#define WSTOPSIG(status)   (((status) >> 8) & 0xff)

#define WIFEXITED(status)  (((status) & 0xff) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xff)

#ifndef _KERNEL
pid_t waitpid(pid_t pid, int* status, int options);
#endif
