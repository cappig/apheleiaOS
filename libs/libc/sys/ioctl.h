#pragma once

#include <sys/types.h>

#define TCGETS     1
#define TCSETS     2
#define TCSETSW    3
#define TCSETSF    4
#define TIOCGWINSZ 5
#define TIOCSWINSZ 6
#define TIOCSPGRP  7
#define TIOCGPGRP  8
#define TIOCGPTN   9

int ioctl(int fd, unsigned long request, ...);
