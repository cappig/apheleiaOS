#pragma once

#include <sys/types.h>
#include <time.h>

// file type
#define S_IFMT 0170000

// socket
#define S_IFSOCK 0140000
// symbolic link
#define S_IFLNK 0120000
// regular
#define S_IFREG 0100000
// block device
#define S_IFBLK 0060000
// directory
#define S_IFDIR 0040000
// character device
#define S_IFCHR 0020000
// fifo
#define S_IFIFO 0010000

// owner
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

// group
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

// others
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

// set UID bit
#define S_ISUID 04000
// set GID bit
#define S_ISGID 02000
// sticky bit
#define S_ISVTX 01000

struct stat {
    dev_t st_dev;
    ino_t st_ino;
    mode_t st_mode;
    nlink_t st_nlink;
    uid_t st_uid;
    gid_t st_gid;
    dev_t st_rdev;
    off_t st_size;
    blksize_t st_blksize;
    blkcnt_t st_blocks;
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
};

#ifdef _APHELEIA_SOURCE
typedef struct stat stat_t;
#endif

#ifndef _KERNEL
int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
int fstat(int fd, struct stat *st);
int chmod(const char *path, mode_t mode);
int chown(const char *path, uid_t uid, gid_t gid);
#endif
