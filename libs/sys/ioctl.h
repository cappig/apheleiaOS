#pragma once

#include <sys/types.h>

#define TCGETS  1
#define TCSETS  2
#define TCSETSW 3
#define TCSETSF 4

#define TIOCGWINSZ 5
#define TIOCSWINSZ 6

struct ioctl {
    size_t len;
    void* args;
};

typedef struct ioctl ioctl_t;


int ioctl(int fd, unsigned long request, struct ioctl* data);
