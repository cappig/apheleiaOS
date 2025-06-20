#pragma once


// File type
#define S_IFMT 0170000

// Socket
#define S_IFSOCK 0140000
// Symbolic link
#define S_IFLNK 0120000
// Regular
#define S_IFREG 0100000
// Block device
#define S_IFBLK 0060000
// Directory
#define S_IFDIR 0040000
// Character device
#define S_IFCHR 0020000
// FIFO
#define S_IFIFO 0010000


// Owner
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

// Group
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

// Others
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001

// Set UID bit
#define S_ISUID 04000
// Set GID bit
#define S_ISGID 02000
// Sticky bit
#define S_ISVTX 01000
