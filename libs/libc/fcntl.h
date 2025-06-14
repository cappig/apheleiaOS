#pragma once

#define O_RDONLY (1 << 0)
#define O_WRONLY (1 << 1)
#define O_RDWR   (1 << 2)

#define O_CREAT    (1 << 6)
#define O_EXCL     (1 << 7)
#define O_NOCTTY   (1 << 8)
#define O_TRUNC    (1 << 9)
#define O_APPEND   (1 << 10)
#define O_NONBLOCK (1 << 11)
#define O_SYNC     (1 << 12)

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#ifndef _KERNEL
#include <libc_usr/fcntl.h>
#endif
