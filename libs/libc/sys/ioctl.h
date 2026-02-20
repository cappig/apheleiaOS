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

#define WSIOC_CLAIM_MANAGER   64
#define WSIOC_RELEASE_MANAGER 65
#define WSIOC_SET_FOCUS       66
#define WSIOC_SET_POS         67
#define WSIOC_SET_Z           68
#define WSIOC_SEND_INPUT      69
#define WSIOC_CLOSE           70
#define WSIOC_ALLOC           71
#define WSIOC_FREE            72
#define WSIOC_QUERY           73
#define WSIOC_SET_SIZE        74
#endif

int ioctl(int fd, unsigned long request, ...);
