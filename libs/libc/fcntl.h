#pragma once

#define O_ACCMODE 00000003
#define O_RDONLY  00000000
#define O_WRONLY  00000001
#define O_RDWR    00000002

#define O_CREAT     00000100
#define O_EXCL      00000200
#define O_NOCTTY    00000400
#define O_TRUNC     00001000
#define O_APPEND    00002000
#define O_NONBLOCK  00004000
#define O_SYNC      00010000
#define O_DIRECTORY 00020000
#define O_CLOEXEC   00040000
#define O_NOFOLLOW  00100000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
