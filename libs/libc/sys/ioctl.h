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

#ifdef _APHELEIA_SOURCE
#include <gui/fb.h>

#define FBIOGETINFO      10
#define FBIOACQUIRE      11
#define FBIORELEASE      12
#define FBIOPRESENT      13
#define FBIOPRESENT_RECT 14

#define WSIOCCLAIMMGR     64
#define WSIOCRELEASEMGR   65
#define WSIOCSFOCUS       66
#define WSIOCSPOS         67
#define WSIOCSZ           68
#define WSIOCSINPUT       69
#define WSIOCCLOSE        70
#define WSIOCALLOC        71
#define WSIOCFREE         72
#define WSIOCGINFO        73
#define WSIOCSSIZE        74
#define WSIOCTRANSFERMGR  75
#define WSIOCSTITLE       76

#define CLOCKIOCGSNAPSHOT 80
#endif

int ioctl(int fd, unsigned long request, ...);
